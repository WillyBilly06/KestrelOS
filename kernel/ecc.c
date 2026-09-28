/* ecc.c - checking signatures made on an elliptic curve.
 *
 * When the certificate work here began, only RSA signatures could be checked,
 * and the client said so in its opening message so that a server would refuse
 * cleanly rather than fail halfway through.  That was honest, and it was also
 * a wall: a large part of the web now presents an elliptic-curve certificate
 * and nothing else, and every one of those sites was simply unreachable.
 *
 * The mathematics is not the same shape as RSA.  There, verifying is one
 * exponentiation and the whole difficulty is in the size of the numbers.  Here
 * the numbers are small - two hundred and fifty-six bits, sometimes three
 * hundred and eighty-four - and the difficulty is that the arithmetic is on
 * points of a curve rather than on numbers.  Adding two points is a defined
 * operation with its own rules; multiplying a point by a number means adding
 * it to itself that many times, done by doubling.
 *
 * A signature is a pair, r and s.  The check reconstructs a point from them
 * and the message, and the signature is good exactly when that point's first
 * coordinate comes back as r.  Only public values are involved - the
 * signature, the public key, the message - so unlike signing, none of this has
 * to be constant-time, and it can be written the straightforward way.
 *
 * Two curves are here: the one nearly everything uses, and the larger one some
 * authorities sign with.  They differ only in their constants and their size,
 * so the code is written once against a curve description.
 */
#include "kernel.h"
#include "crypto.h"
#include "ecc.h"

#define MAX_LIMBS 12                    /* three hundred and eighty-four bits */

typedef struct {
    u32 v[MAX_LIMBS];
} num_t;

typedef struct {
    int limbs;
    num_t p;                            /* the field */
    num_t n;                            /* the order of the group */
    num_t b;                            /* the curve's constant; a is -3 */
    num_t gx, gy;                       /* the point everything starts from */

    /* Montgomery constants, worked out once at first use. */
    u32   p_n0inv, n_n0inv;
    num_t p_rr, n_rr;                   /* R squared, for converting in */
    bool  ready;
} curve_t;

/* ------------------------------------------------------------- plain numbers */

static void num_zero(num_t *a) { memset(a->v, 0, sizeof a->v); }

static bool num_is_zero(const num_t *a, int limbs) {
    u32 combined = 0;
    for (int i = 0; i < limbs; i++) combined |= a->v[i];
    return combined == 0;
}

static int num_compare(const num_t *a, const num_t *b, int limbs) {
    for (int i = limbs - 1; i >= 0; i--)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}

/* Returns the carry out, which the callers below need. */
static u32 num_add(num_t *out, const num_t *a, const num_t *b, int limbs) {
    u64 carry = 0;
    for (int i = 0; i < limbs; i++) {
        u64 sum = (u64)a->v[i] + b->v[i] + carry;
        out->v[i] = (u32)sum;
        carry = sum >> 32;
    }
    return (u32)carry;
}

/* And the borrow out. */
static u32 num_sub(num_t *out, const num_t *a, const num_t *b, int limbs) {
    u64 borrow = 0;
    for (int i = 0; i < limbs; i++) {
        u64 diff = (u64)a->v[i] - b->v[i] - borrow;
        out->v[i] = (u32)diff;
        borrow = (diff >> 32) & 1;
    }
    return (u32)borrow;
}

static void num_from_bytes(num_t *out, const u8 *data, size_t len, int limbs) {
    num_zero(out);
    for (size_t i = 0; i < len; i++) {
        size_t from_end = len - 1 - i;
        if (from_end / 4 >= (size_t)limbs) continue;
        out->v[from_end / 4] |= (u32)data[i] << (8 * (from_end % 4));
    }
}


/* --------------------------------------------------------- modular numbers
 *
 * Everything below works in Montgomery form, for the same reason bignum.c does:
 * it turns the reduction after each multiplication from a division into a
 * shift.  Addition and subtraction are unaffected by the representation, so
 * they are written plainly with a conditional correction.
 */

static void mod_add(num_t *out, const num_t *a, const num_t *b,
                    const num_t *m, int limbs) {
    u32 carry = num_add(out, a, b, limbs);
    num_t reduced;
    u32 borrow = num_sub(&reduced, out, m, limbs);
    /* Take the reduced value when the sum overflowed, or when it did not go
     * negative - either way it is the one in range. */
    if (carry || !borrow) *out = reduced;
}

static void mod_sub(num_t *out, const num_t *a, const num_t *b,
                    const num_t *m, int limbs) {
    u32 borrow = num_sub(out, a, b, limbs);
    if (borrow) num_add(out, out, m, limbs);
}

static u32 mont_inverse(u32 m0) {
    u32 inverse = m0;
    for (int i = 0; i < 5; i++) inverse *= 2 - m0 * inverse;
    return (u32)(0 - inverse);
}

/* out = a * b * R^-1 mod m. */
static void mont_mul(num_t *out, const num_t *a, const num_t *b,
                     const num_t *m, u32 n0inv, int limbs) {
    u32 t[MAX_LIMBS + 2];
    memset(t, 0, sizeof t);

    for (int i = 0; i < limbs; i++) {
        u64 carry = 0;
        for (int j = 0; j < limbs; j++) {
            u64 sum = (u64)t[j] + (u64)a->v[i] * b->v[j] + carry;
            t[j] = (u32)sum;
            carry = sum >> 32;
        }
        u64 sum = (u64)t[limbs] + carry;
        t[limbs] = (u32)sum;
        t[limbs + 1] += (u32)(sum >> 32);

        u32 factor = t[0] * n0inv;
        carry = 0;
        for (int j = 0; j < limbs; j++) {
            u64 total = (u64)t[j] + (u64)factor * m->v[j] + carry;
            t[j] = (u32)total;
            carry = total >> 32;
        }
        sum = (u64)t[limbs] + carry;
        t[limbs] = (u32)sum;
        t[limbs + 1] += (u32)(sum >> 32);

        for (int j = 0; j <= limbs; j++) t[j] = t[j + 1];
        t[limbs + 1] = 0;
    }

    num_t result;
    num_zero(&result);
    for (int i = 0; i < limbs; i++) result.v[i] = t[i];

    num_t reduced;
    u32 borrow = num_sub(&reduced, &result, m, limbs);
    *out = (t[limbs] || !borrow) ? reduced : result;
}

/* a^(m-2) mod m, which is the inverse when m is prime.  The exponent is public
 * and fixed, so square-and-multiply from the top bit down is fine. */
static void mod_invert(num_t *out, const num_t *a, const num_t *m,
                       u32 n0inv, const num_t *rr, int limbs) {
    num_t exponent, two, base, result, one;

    num_zero(&two);
    two.v[0] = 2;
    num_sub(&exponent, m, &two, limbs);

    mont_mul(&base, a, rr, m, n0inv, limbs);    /* into the representation */

    num_zero(&one);
    one.v[0] = 1;
    mont_mul(&result, &one, rr, m, n0inv, limbs);

    int top = limbs * 32 - 1;
    while (top > 0 && !((exponent.v[top / 32] >> (top % 32)) & 1)) top--;

    for (int bit = top; bit >= 0; bit--) {
        mont_mul(&result, &result, &result, m, n0inv, limbs);
        if ((exponent.v[bit / 32] >> (bit % 32)) & 1)
            mont_mul(&result, &result, &base, m, n0inv, limbs);
    }

    mont_mul(out, &result, &one, m, n0inv, limbs);   /* and back out */
}

/* R squared mod m, built by doubling.  Needed to convert into the
 * representation and computed once per curve. */
static void compute_rr(num_t *out, const num_t *m, int limbs) {
    num_t value;
    num_zero(&value);
    value.v[0] = 1;

    for (int i = 0; i < limbs * 32 * 2; i++) {
        u32 carry = num_add(&value, &value, &value, limbs);
        num_t reduced;
        u32 borrow = num_sub(&reduced, &value, m, limbs);
        if (carry || !borrow) value = reduced;
    }
    *out = value;
}

/* ------------------------------------------------------------------ points
 *
 * Points are held with three coordinates rather than two.  The reason is that
 * adding two points in the ordinary two-coordinate form needs a division, and
 * a division here means a modular inverse, which costs more than everything
 * else in the operation put together.  Carrying a denominator along instead
 * defers all of them to one inverse at the very end.
 *
 * A point at infinity - the identity, the result of adding a point to its own
 * negation - is the one with a zero denominator.
 */
typedef struct {
    num_t x, y, z;
} point_t;

static bool point_is_infinity(const point_t *p, int limbs) {
    return num_is_zero(&p->z, limbs);
}

static void point_set_infinity(point_t *p) {
    num_zero(&p->x); num_zero(&p->y); num_zero(&p->z);
    p->x.v[0] = 1;
    p->y.v[0] = 1;
}

/* Doubling, with the curve's constant a fixed at -3 - which is true of both
 * curves here and of essentially every curve in use, precisely because it
 * makes this shorter. */
static void point_double(point_t *out, const point_t *in, const curve_t *c) {
    int limbs = c->limbs;
    const num_t *p = &c->p;
    u32 n0 = c->p_n0inv;

    if (point_is_infinity(in, limbs) || num_is_zero(&in->y, limbs)) {
        point_set_infinity(out);
        return;
    }

    num_t a, b, d, e, f, t1, t2;

    mont_mul(&a, &in->y, &in->y, p, n0, limbs);          /* y^2 */
    mont_mul(&b, &in->x, &a, p, n0, limbs);
    mod_add(&b, &b, &b, p, limbs);
    mod_add(&b, &b, &b, p, limbs);                        /* 4xy^2 */

    mont_mul(&d, &a, &a, p, n0, limbs);                   /* y^4 */
    mod_add(&d, &d, &d, p, limbs);
    mod_add(&d, &d, &d, p, limbs);
    mod_add(&d, &d, &d, p, limbs);                        /* 8y^4 */

    mont_mul(&t1, &in->z, &in->z, p, n0, limbs);          /* z^2 */
    mod_sub(&t2, &in->x, &t1, p, limbs);
    mod_add(&e, &in->x, &t1, p, limbs);
    mont_mul(&e, &e, &t2, p, n0, limbs);                  /* (x-z^2)(x+z^2) */
    mod_add(&t2, &e, &e, p, limbs);
    mod_add(&e, &e, &t2, p, limbs);                       /* 3(x^2 - z^4) */

    mont_mul(&f, &e, &e, p, n0, limbs);
    mod_sub(&f, &f, &b, p, limbs);
    mod_sub(&f, &f, &b, p, limbs);                        /* x' */

    mont_mul(&t1, &in->y, &in->z, p, n0, limbs);
    mod_add(&t1, &t1, &t1, p, limbs);                     /* z' */

    mod_sub(&t2, &b, &f, p, limbs);
    mont_mul(&t2, &e, &t2, p, n0, limbs);
    mod_sub(&t2, &t2, &d, p, limbs);                      /* y' */

    out->x = f;
    out->y = t2;
    out->z = t1;
}

/* Adding two points.  The case where they turn out to be the same point needs
 * the doubling formula instead - the general one divides by zero there. */
static void point_add(point_t *out, const point_t *a, const point_t *b,
                      const curve_t *c) {
    int limbs = c->limbs;
    const num_t *p = &c->p;
    u32 n0 = c->p_n0inv;

    if (point_is_infinity(a, limbs)) { *out = *b; return; }
    if (point_is_infinity(b, limbs)) { *out = *a; return; }

    num_t z1z1, z2z2, u1, u2, s1, s2, h, r, t1, t2, t3;

    mont_mul(&z1z1, &a->z, &a->z, p, n0, limbs);
    mont_mul(&z2z2, &b->z, &b->z, p, n0, limbs);

    mont_mul(&u1, &a->x, &z2z2, p, n0, limbs);
    mont_mul(&u2, &b->x, &z1z1, p, n0, limbs);

    mont_mul(&t1, &b->z, &z2z2, p, n0, limbs);
    mont_mul(&s1, &a->y, &t1, p, n0, limbs);
    mont_mul(&t1, &a->z, &z1z1, p, n0, limbs);
    mont_mul(&s2, &b->y, &t1, p, n0, limbs);

    mod_sub(&h, &u2, &u1, p, limbs);
    mod_sub(&r, &s2, &s1, p, limbs);

    if (num_is_zero(&h, limbs)) {
        if (num_is_zero(&r, limbs)) point_double(out, a, c);
        else point_set_infinity(out);
        return;
    }

    mont_mul(&t1, &h, &h, p, n0, limbs);                  /* h^2 */
    mont_mul(&t2, &t1, &h, p, n0, limbs);                 /* h^3 */
    mont_mul(&t3, &u1, &t1, p, n0, limbs);                /* u1 h^2 */

    mont_mul(&out->x, &r, &r, p, n0, limbs);
    mod_sub(&out->x, &out->x, &t2, p, limbs);
    mod_sub(&out->x, &out->x, &t3, p, limbs);
    mod_sub(&out->x, &out->x, &t3, p, limbs);

    mod_sub(&t3, &t3, &out->x, p, limbs);
    mont_mul(&t3, &r, &t3, p, n0, limbs);
    mont_mul(&t2, &s1, &t2, p, n0, limbs);
    mod_sub(&out->y, &t3, &t2, p, limbs);

    mont_mul(&t1, &a->z, &b->z, p, n0, limbs);
    mont_mul(&out->z, &t1, &h, p, n0, limbs);
}

/* Multiplying a point by a number, by doubling.  Everything involved here is
 * public, so the plain form - which takes a different path depending on the
 * bits of the multiplier - gives nothing away. */
static void point_multiply(point_t *out, const point_t *base,
                           const num_t *scalar, const curve_t *c) {
    point_t result;
    point_set_infinity(&result);

    int top = c->limbs * 32 - 1;
    while (top > 0 && !((scalar->v[top / 32] >> (top % 32)) & 1)) top--;

    for (int bit = top; bit >= 0; bit--) {
        point_double(&result, &result, c);
        if ((scalar->v[bit / 32] >> (bit % 32)) & 1)
            point_add(&result, &result, base, c);
    }
    *out = result;
}

/* --------------------------------------------------------------- the curves */

static curve_t curve_p256 = {
    .limbs = 8,
    .p  = {{ 0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0x00000000,
             0x00000000,0x00000000,0x00000001,0xFFFFFFFF }},
    .n  = {{ 0xFC632551,0xF3B9CAC2,0xA7179E84,0xBCE6FAAD,
             0xFFFFFFFF,0xFFFFFFFF,0x00000000,0xFFFFFFFF }},
    .b  = {{ 0x27D2604B,0x3BCE3C3E,0xCC53B0F6,0x651D06B0,
             0x769886BC,0xB3EBBD55,0xAA3A93E7,0x5AC635D8 }},
    .gx = {{ 0xD898C296,0xF4A13945,0x2DEB33A0,0x77037D81,
             0x63A440F2,0xF8BCE6E5,0xE12C4247,0x6B17D1F2 }},
    .gy = {{ 0x37BF51F5,0xCBB64068,0x6B315ECE,0x2BCE3357,
             0x7C0F9E16,0x8EE7EB4A,0xFE1A7F9B,0x4FE342E2 }},
};

static curve_t curve_p384 = {
    .limbs = 12,
    .p  = {{ 0xFFFFFFFF,0x00000000,0x00000000,0xFFFFFFFF,
             0xFFFFFFFE,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,
             0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF }},
    .n  = {{ 0xCCC52973,0xECEC196A,0x48B0A77A,0x581A0DB2,
             0xF4372DDF,0xC7634D81,0xFFFFFFFF,0xFFFFFFFF,
             0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF }},
    .b  = {{ 0xD3EC2AEF,0x2A85C8ED,0x8A2ED19D,0xC656398D,
             0x5013875A,0x0314088F,0xFE814112,0x181D9C6E,
             0xE3F82D19,0x988E056B,0xE23EE7E4,0xB3312FA7 }},
    .gx = {{ 0x72760AB7,0x3A545E38,0xBF55296C,0x5502F25D,
             0x82542A38,0x59F741E0,0x8BA79B98,0x6E1D3B62,
             0xF320AD74,0x8EB1C71E,0xBE8B0537,0xAA87CA22 }},
    .gy = {{ 0x90EA0E5F,0x7A431D7C,0x1D7E819D,0x0A60B1CE,
             0xB5F0B8C0,0xE9DA3113,0x289A147C,0xF8F41DBD,
             0x9292DC29,0x5D9E98BF,0x96262C6F,0x3617DE4A }},
};

static void prepare(curve_t *c) {
    if (c->ready) return;
    c->p_n0inv = mont_inverse(c->p.v[0]);
    c->n_n0inv = mont_inverse(c->n.v[0]);
    compute_rr(&c->p_rr, &c->p, c->limbs);
    compute_rr(&c->n_rr, &c->n, c->limbs);
    c->ready = true;
}

static curve_t *curve_for(ecc_curve which) {
    switch (which) {
    case ECC_P256: prepare(&curve_p256); return &curve_p256;
    case ECC_P384: prepare(&curve_p384); return &curve_p384;
    default: return NULL;
    }
}

int ecc_field_bytes(ecc_curve which) {
    switch (which) {
    case ECC_P256: return 32;
    case ECC_P384: return 48;
    default: return 0;
    }
}

/* ---------------------------------------------------------------- verifying
 *
 * The check, in full:
 *
 *   both r and s must be in range - not zero, less than the group's order;
 *   w  = s^-1
 *   u1 = e * w,  u2 = r * w      (e is the message hash, as a number)
 *   R  = u1*G + u2*Q             (G the curve's base point, Q the public key)
 *   the signature is good exactly when R's first coordinate equals r.
 *
 * Every step matters.  Skipping the range checks accepts signatures that are
 * not signatures; forgetting that R may come out as the identity accepts a
 * forged pair; comparing the wrong coordinate accepts anything at all.
 */
bool ecc_verify(ecc_curve which, const u8 *public_key, size_t public_key_len,
                const u8 *hash, size_t hash_len,
                const u8 *r_bytes, size_t r_len,
                const u8 *s_bytes, size_t s_len) {
    curve_t *c = curve_for(which);
    if (!c) return false;

    int limbs = c->limbs;
    size_t field = (size_t)ecc_field_bytes(which);

    /* The key arrives as the two coordinates, preceded by a byte saying which
     * of the several encodings it is.  Only the plain one is accepted; the
     * compressed forms would need a square root to undo and are not used by
     * anything that issues certificates. */
    if (public_key_len != 1 + 2 * field || public_key[0] != 0x04) return false;

    num_t r, s;
    num_from_bytes(&r, r_bytes, r_len, limbs);
    num_from_bytes(&s, s_bytes, s_len, limbs);

    if (num_is_zero(&r, limbs) || num_is_zero(&s, limbs)) return false;
    if (num_compare(&r, &c->n, limbs) >= 0) return false;
    if (num_compare(&s, &c->n, limbs) >= 0) return false;

    /* The hash as a number.  When it is longer than the group's order, the
     * leftmost bits are the ones used - so a longer hash with a shorter curve
     * is truncated rather than reduced. */
    num_t e;
    if (hash_len > field) hash_len = field;
    num_from_bytes(&e, hash, hash_len, limbs);
    if (hash_len * 8 > (size_t)(limbs * 32)) return false;
    if (num_compare(&e, &c->n, limbs) >= 0) num_sub(&e, &e, &c->n, limbs);

    num_t w;
    mod_invert(&w, &s, &c->n, c->n_n0inv, &c->n_rr, limbs);

    /* Two products modulo the order.  Converting in and out of the
     * representation around each is what keeps this readable; the cost is
     * three multiplications where two would do. */
    num_t u1, u2, tmp;
    mont_mul(&tmp, &e, &c->n_rr, &c->n, c->n_n0inv, limbs);
    mont_mul(&u1, &tmp, &w, &c->n, c->n_n0inv, limbs);

    mont_mul(&tmp, &r, &c->n_rr, &c->n, c->n_n0inv, limbs);
    mont_mul(&u2, &tmp, &w, &c->n, c->n_n0inv, limbs);

    /* The base point and the public key, both into the representation the
     * point arithmetic works in. */
    point_t g, q;
    mont_mul(&g.x, &c->gx, &c->p_rr, &c->p, c->p_n0inv, limbs);
    mont_mul(&g.y, &c->gy, &c->p_rr, &c->p, c->p_n0inv, limbs);
    num_zero(&g.z);
    g.z.v[0] = 1;
    mont_mul(&g.z, &g.z, &c->p_rr, &c->p, c->p_n0inv, limbs);

    num_t qx, qy;
    num_from_bytes(&qx, public_key + 1, field, limbs);
    num_from_bytes(&qy, public_key + 1 + field, field, limbs);

    /* A key whose coordinates are not in the field is not a key. */
    if (num_compare(&qx, &c->p, limbs) >= 0) return false;
    if (num_compare(&qy, &c->p, limbs) >= 0) return false;

    mont_mul(&q.x, &qx, &c->p_rr, &c->p, c->p_n0inv, limbs);
    mont_mul(&q.y, &qy, &c->p_rr, &c->p, c->p_n0inv, limbs);
    q.z = g.z;

    /* Is the key even on the curve?  A point that is not satisfies no equation
     * the arithmetic below relies on, and accepting one opens the door to
     * signatures that verify against a key the signer never had.  y^2 must
     * equal x^3 - 3x + b. */
    {
        num_t left, right, three_x, bm;
        mont_mul(&left, &q.y, &q.y, &c->p, c->p_n0inv, limbs);
        mont_mul(&right, &q.x, &q.x, &c->p, c->p_n0inv, limbs);
        mont_mul(&right, &right, &q.x, &c->p, c->p_n0inv, limbs);
        mod_add(&three_x, &q.x, &q.x, &c->p, limbs);
        mod_add(&three_x, &three_x, &q.x, &c->p, limbs);
        mod_sub(&right, &right, &three_x, &c->p, limbs);
        mont_mul(&bm, &c->b, &c->p_rr, &c->p, c->p_n0inv, limbs);
        mod_add(&right, &right, &bm, &c->p, limbs);
        if (num_compare(&left, &right, limbs) != 0) return false;
    }

    point_t first, second, sum;
    point_multiply(&first, &g, &u1, c);
    point_multiply(&second, &q, &u2, c);
    point_add(&sum, &first, &second, c);

    /* The identity has no first coordinate, so nothing can equal r. */
    if (point_is_infinity(&sum, limbs)) return false;

    /* Back to ordinary coordinates: divide by the denominator squared.  This
     * is the one inverse the whole calculation needed. */
    num_t z_inv, z_inv2, x;
    num_t z_plain, one;
    num_zero(&one);
    one.v[0] = 1;
    mont_mul(&z_plain, &sum.z, &one, &c->p, c->p_n0inv, limbs);
    mod_invert(&z_inv, &z_plain, &c->p, c->p_n0inv, &c->p_rr, limbs);

    mont_mul(&z_inv, &z_inv, &c->p_rr, &c->p, c->p_n0inv, limbs);
    mont_mul(&z_inv2, &z_inv, &z_inv, &c->p, c->p_n0inv, limbs);
    mont_mul(&x, &sum.x, &z_inv2, &c->p, c->p_n0inv, limbs);
    mont_mul(&x, &x, &one, &c->p, c->p_n0inv, limbs);

    /* And the comparison is modulo the group's order, not the field's. */
    if (num_compare(&x, &c->n, limbs) >= 0) num_sub(&x, &x, &c->n, limbs);

    return num_compare(&x, &r, limbs) == 0;
}

/* A signature as a certificate stores it: a sequence of two integers.  Reading
 * it here rather than in the certificate parser keeps the two curves' quirks -
 * the leading zero a positive number needs, the short forms - in one place. */
bool ecc_split_signature(const u8 *der, size_t len,
                         const u8 **r, size_t *r_len,
                         const u8 **s, size_t *s_len) {
    if (len < 8 || der[0] != 0x30) return false;

    size_t at = 1;
    size_t seq_len;
    if (der[at] < 0x80) { seq_len = der[at]; at++; }
    else {
        int count = der[at] & 0x7F;
        at++;
        if (count < 1 || count > 2 || at + (size_t)count > len) return false;
        seq_len = 0;
        for (int i = 0; i < count; i++) seq_len = (seq_len << 8) | der[at++];
    }
    if (at + seq_len > len) return false;
    size_t end = at + seq_len;

    for (int which = 0; which < 2; which++) {
        if (at + 2 > end || der[at] != 0x02) return false;
        at++;
        size_t size = der[at++];
        if (size > 0x7F || size == 0) return false;   /* no integer here is that big */
        if (at + size > end) return false;

        const u8 *value = der + at;
        size_t value_len = size;
        at += size;                       /* advanced before the value is trimmed */

        /* A positive number whose top bit is set carries a leading zero so it
         * is not read as negative; it is not part of the value. */
        while (value_len > 1 && value[0] == 0) { value++; value_len--; }

        if (which == 0) { *r = value; *r_len = value_len; }
        else            { *s = value; *s_len = value_len; }
    }
    return true;
}

/* ------------------------------------------------------------------- test */

int ecc_selftest(void) {
    int failures = 0;

    /* A signature made outside this system, on a key made outside this system,
     * over a message this system did not choose.  Checking it exercises every
     * part of the calculation at once - the field arithmetic, the point
     * addition, the doubling, the inverse, the final comparison - and any one
     * of them being wrong shows up here rather than as a site that will not
     * load for reasons nobody can see.
     */
    {
        static const u8 key[65] = {
            0x04,0x6C,0x4E,0xFF,0x53,0xD6,0x4C,0xCB,0x7A,0xF8,0xCD,0xC6,0xC7,0x92,0x73,0xED,
            0x34,0x32,0xA5,0xDF,0x8A,0xC7,0x94,0x62,0x90,0xCE,0xC8,0x2A,0x8A,0xBD,0x7F,0x45,
            0xE0,0xBF,0xB7,0x57,0xAB,0xB5,0x97,0x96,0x57,0xFE,0xBE,0xBB,0xB1,0x69,0x55,0x1E,
            0xC5,0x9C,0x55,0xB7,0xA2,0x4D,0x85,0xAF,0x3E,0xD2,0xE6,0x80,0x8B,0x5E,0xD0,0x18,
            0x23,
        };
        static const u8 hash[32] = {
            0x5D,0x7E,0x1A,0xDC,0x43,0x7F,0x42,0x57,0x57,0xCC,0xEE,0xC3,0x45,0xD0,0x84,0xDF,
            0xA0,0x67,0xC4,0x9D,0x2B,0x4F,0xEA,0xB6,0x1A,0x2B,0xD5,0xD3,0xC4,0x66,0xF0,0x30,
        };
        static const u8 r[32] = {
            0xFE,0xD4,0xE7,0x51,0x55,0xE5,0xED,0x78,0x97,0x63,0xB3,0x3D,0x1E,0xCD,0x27,0xFB,
            0x33,0x99,0xA0,0x98,0x34,0x71,0x49,0xD6,0x92,0x9A,0xAE,0x30,0x4D,0x60,0xE0,0x65,
        };
        static const u8 s[32] = {
            0xFC,0xF7,0x16,0x3A,0xE0,0x4A,0x82,0x73,0x73,0xA6,0x09,0x52,0xAD,0x49,0x53,0x5C,
            0x68,0x72,0x64,0x8D,0xFA,0xE7,0x62,0x2B,0x08,0xFB,0x12,0x23,0x54,0xE9,0x4B,0xD3,
        };

        if (!ecc_verify(ECC_P256, key, sizeof key, hash, sizeof hash,
                        r, sizeof r, s, sizeof s)) {
            kerr("ecc", "the P-256 signature did not verify");
            failures++;
        }

        /* An altered message has to fail.  If it does not, the signature is
         * not being checked against the message at all - which is a validator
         * that says yes to everything. */
        u8 changed[32];
        memcpy(changed, hash, sizeof changed);
        changed[0] ^= 0x01;
        if (ecc_verify(ECC_P256, key, sizeof key, changed, sizeof changed,
                       r, sizeof r, s, sizeof s)) {
            kerr("ecc", "a signature verified against the wrong message");
            failures++;
        }

        /* So does an altered signature. */
        u8 bad_r[32];
        memcpy(bad_r, r, sizeof bad_r);
        bad_r[31] ^= 0x01;
        if (ecc_verify(ECC_P256, key, sizeof key, hash, sizeof hash,
                       bad_r, sizeof bad_r, s, sizeof s)) {
            kerr("ecc", "an altered signature was accepted");
            failures++;
        }

        /* And a key that is not a point on the curve.  Accepting one of those
         * opens the door to signatures that verify against a key nobody
         * holds. */
        u8 bad_key[65];
        memcpy(bad_key, key, sizeof bad_key);
        bad_key[64] ^= 0x01;
        if (ecc_verify(ECC_P256, bad_key, sizeof bad_key, hash, sizeof hash,
                       r, sizeof r, s, sizeof s)) {
            kerr("ecc", "a point that is not on the curve was accepted as a key");
            failures++;
        }
    }

    /* The larger curve, which some authorities sign with. */
    {
        static const u8 key[97] = {
            0x04,0xBB,0xA9,0x25,0x13,0x22,0x86,0x32,0x34,0xA6,0xDE,0xF6,0xFC,0x05,0x32,0x2A,
            0x16,0xCA,0xC0,0x6B,0xAE,0xAE,0x55,0xF5,0xA2,0xCD,0x95,0x12,0x5E,0x30,0xF9,0x2F,
            0xB1,0xCF,0x52,0x3B,0x0D,0x75,0x02,0x11,0x72,0x66,0x3D,0xE8,0xE5,0x7B,0x52,0x95,
            0xB3,0xF2,0x74,0x11,0x00,0x1F,0x33,0x1D,0xFD,0xD5,0xF8,0x67,0x8A,0x03,0xAF,0x39,
            0x39,0x53,0x98,0xBC,0xE8,0x7E,0x08,0xB4,0xB4,0x99,0x26,0x86,0x19,0x58,0xDC,0x53,
            0xA5,0x6E,0xCE,0xB5,0xC1,0x9C,0x67,0x79,0xD1,0x7C,0x6A,0x5C,0x66,0x94,0xDD,0x32,
            0xE0,
        };
        static const u8 hash[48] = {
            0x36,0xE3,0x64,0x01,0x8B,0xC8,0x9A,0x5E,0x05,0xFB,0x60,0x26,0xB2,0xAA,0x64,0x60,
            0xE0,0x83,0xA3,0x95,0xF4,0x39,0x4E,0xA3,0x82,0x54,0x6C,0xE9,0x4E,0x11,0x18,0x3E,
            0x9F,0x21,0x3A,0x80,0x8F,0x50,0x4D,0xAD,0x39,0xBA,0x66,0x8F,0x5A,0x5E,0x39,0xCD,
        };
        static const u8 r[48] = {
            0x2B,0x6D,0x52,0xB9,0x44,0xA0,0x6D,0x1B,0x1F,0x07,0x4F,0x04,0x44,0x88,0x1F,0xFF,
            0xEA,0xFE,0x99,0x46,0xFC,0xB7,0x60,0x16,0xE2,0x95,0x18,0x10,0x88,0x84,0x02,0x8C,
            0x7A,0x96,0xA5,0x4B,0x21,0x91,0x2D,0xBA,0x8E,0x83,0x1B,0x47,0x7C,0x76,0xF1,0x38,
        };
        static const u8 s[48] = {
            0xEF,0x70,0x29,0x1C,0x3B,0x77,0xC0,0x19,0x7F,0x0C,0xE7,0xB0,0xC1,0x8D,0xEC,0x90,
            0xD8,0x3C,0x26,0x0E,0x03,0x1B,0xDA,0x4C,0x9E,0xEE,0x2C,0x63,0x13,0xEE,0xF9,0xB0,
            0x0F,0xE3,0x45,0x36,0x41,0x89,0xD7,0xF5,0xB2,0x43,0xA4,0xF4,0x6A,0xCC,0x47,0x78,
        };

        if (!ecc_verify(ECC_P384, key, sizeof key, hash, sizeof hash,
                        r, sizeof r, s, sizeof s)) {
            kerr("ecc", "the P-384 signature did not verify");
            failures++;
        }

        u8 changed[48];
        memcpy(changed, hash, sizeof changed);
        changed[17] ^= 0x40;
        if (ecc_verify(ECC_P384, key, sizeof key, changed, sizeof changed,
                       r, sizeof r, s, sizeof s)) {
            kerr("ecc", "a P-384 signature verified against the wrong message");
            failures++;
        }
    }

    /* And the encoded form a certificate stores a signature in. */
    {
        static const u8 encoded[] = {
            0x30, 0x0C,
            0x02, 0x03, 0x01, 0x02, 0x03,
            0x02, 0x05, 0x00, 0xFF, 0x00, 0x11, 0x22,
        };
        const u8 *r = NULL, *s = NULL;
        size_t r_len = 0, s_len = 0;
        if (!ecc_split_signature(encoded, sizeof encoded, &r, &r_len, &s, &s_len)) {
            kerr("ecc", "a well-formed encoded signature was refused");
            failures++;
        } else if (r_len != 3 || r[0] != 0x01 || s_len != 4 || s[0] != 0xFF) {
            /* The second value has a leading zero because its top bit is set;
             * that zero is padding and not part of the number. */
            kerr("ecc", "an encoded signature was split wrongly (%zu and %zu bytes)",
                 r_len, s_len);
            failures++;
        }
    }

    if (!failures)
        kinfo("crypto", "elliptic-curve signatures on P-256 and P-384 verify, "
                        "and altered ones and off-curve keys are refused");
    return failures;
}
