/* bignum.c - arithmetic on numbers far too big for a register.
 *
 * A certificate is signed with RSA, and checking that signature means raising a
 * two-thousand-bit number to a power modulo another two-thousand-bit number.
 * There is no way round the arithmetic: it is what the signature is.
 *
 * The only hard part is the reduction.  Dividing a four-thousand-bit product by
 * a two-thousand-bit modulus, over and over, is slow and fiddly.  Montgomery's
 * method avoids division entirely: it works in a representation where reducing
 * is a multiplication and a shift, and the cost of converting in and out is
 * paid once.  Everything below is in that representation between the two
 * conversions.
 *
 * Numbers are arrays of thirty-two bit limbs, least significant first, which
 * is the order the arithmetic wants even though certificates store them the
 * other way round.
 */
#include "kernel.h"
#include "crypto.h"

#define BN_LIMBS 128            /* four thousand and ninety-six bits */

typedef struct {
    /* Two limbs of headroom past the largest modulus.  Doubling a number that
     * already fills the array produces one more limb, and a carry that has
     * nowhere to go is not an overflow that crashes - it is a silently wrong
     * answer, which is worse.  A four-thousand-bit key is exactly the size
     * where that happens, so it is the size at which a missing limb here would
     * show up as some certificates verifying and others not. */
    u32 limb[BN_LIMBS + 2];
    int used;                   /* how many limbs are significant */
} bignum_t;

static void bn_zero(bignum_t *a) {
    memset(a->limb, 0, sizeof a->limb);
    a->used = 0;
}

static void bn_trim(bignum_t *a) {
    while (a->used > 0 && a->limb[a->used - 1] == 0) a->used--;
}

/* Reading the big-endian form a certificate stores. */
static bool bn_from_bytes(bignum_t *a, const u8 *data, size_t len) {
    bn_zero(a);
    /* Leading zeros carry no value and only cost limbs. */
    while (len && *data == 0) { data++; len--; }
    if (len > BN_LIMBS * 4) return false;

    /* The byte `i` places from the end carries a weight of two to the power of
     * eight times that, so it belongs in limb (i / 4) at byte position (i % 4).
     * Getting this the other way round produces a number that is wrong in a
     * way every subsequent step preserves. */
    for (size_t i = 0; i < len; i++) {
        size_t from_end = len - 1 - i;
        a->limb[from_end / 4] |= (u32)data[i] << (8 * (from_end % 4));
    }
    a->used = (int)((len + 3) / 4);
    bn_trim(a);
    return true;
}

/* And writing it back, padded on the left to a fixed width. */
static void bn_to_bytes(const bignum_t *a, u8 *out, size_t len) {
    memset(out, 0, len);
    for (size_t i = 0; i < len; i++) {
        size_t from_end = len - 1 - i;
        if (from_end / 4 >= BN_LIMBS) continue;
        out[i] = (u8)(a->limb[from_end / 4] >> (8 * (from_end % 4)));
    }
}

static int bn_compare(const bignum_t *a, const bignum_t *b) {
    int top = a->used > b->used ? a->used : b->used;
    for (int i = top - 1; i >= 0; i--) {
        u32 x = i < a->used ? a->limb[i] : 0;
        u32 y = i < b->used ? b->limb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

/* a = a - b, assuming a is at least b. */
static void bn_sub(bignum_t *a, const bignum_t *b) {
    u64 borrow = 0;
    for (int i = 0; i < a->used; i++) {
        u64 y = (i < b->used ? b->limb[i] : 0) + borrow;
        u64 x = a->limb[i];
        if (x < y) { a->limb[i] = (u32)(x + 0x100000000ULL - y); borrow = 1; }
        else { a->limb[i] = (u32)(x - y); borrow = 0; }
    }
    bn_trim(a);
}

/* a = a * 2, modulo n.  Used to build the conversion constant. */
static void bn_double_mod(bignum_t *a, const bignum_t *n) {
    u32 carry = 0;
    for (int i = 0; i < n->used; i++) {
        u32 next = a->limb[i] >> 31;
        a->limb[i] = (a->limb[i] << 1) | carry;
        carry = next;
    }
    if (carry) {
        a->limb[n->used] = carry;
        a->used = n->used + 1;
    } else {
        a->used = n->used;
    }
    bn_trim(a);
    if (bn_compare(a, n) >= 0) bn_sub(a, n);
}

/* ------------------------------------------------------------- Montgomery
 *
 * In this representation a number x is held as x·R mod n, where R is two to
 * the power of the modulus's bit length rounded up to a limb boundary.  The
 * product of two such numbers, divided by R, is the representation of their
 * product - and dividing by R is a shift, because R is a power of the limb
 * size.  That is the whole trick: no division anywhere.
 */

/* The inverse of the modulus's lowest limb, negated, modulo the limb size.
 * Newton's method doubles the number of correct bits each time, so five steps
 * covers thirty-two. */
static u32 mont_n0inv(u32 n0) {
    u32 inverse = n0;                    /* correct to three bits already */
    for (int i = 0; i < 5; i++) inverse *= 2 - n0 * inverse;
    return (u32)(0 - inverse);
}

/* out = a * b * R^-1 mod n, interleaving the multiplication and the reduction
 * so nothing ever grows beyond one limb past the modulus. */
static void mont_mul(bignum_t *out, const bignum_t *a, const bignum_t *b,
                     const bignum_t *n, u32 n0inv) {
    int size = n->used;
    u32 t[BN_LIMBS + 2];
    memset(t, 0, (size_t)(size + 2) * sizeof(u32));

    for (int i = 0; i < size; i++) {
        u32 ai = i < a->used ? a->limb[i] : 0;

        /* t += a_i * b */
        u64 carry = 0;
        for (int j = 0; j < size; j++) {
            u64 bj = j < b->used ? b->limb[j] : 0;
            u64 sum = (u64)t[j] + (u64)ai * bj + carry;
            t[j] = (u32)sum;
            carry = sum >> 32;
        }
        u64 sum = (u64)t[size] + carry;
        t[size] = (u32)sum;
        t[size + 1] += (u32)(sum >> 32);

        /* And the reduction step: add a multiple of n chosen to make the
         * lowest limb zero, then shift the whole thing down by one limb. */
        u32 m = t[0] * n0inv;
        carry = 0;
        for (int j = 0; j < size; j++) {
            u64 total = (u64)t[j] + (u64)m * n->limb[j] + carry;
            t[j] = (u32)total;
            carry = total >> 32;
        }
        sum = (u64)t[size] + carry;
        t[size] = (u32)sum;
        t[size + 1] += (u32)(sum >> 32);

        for (int j = 0; j <= size; j++) t[j] = t[j + 1];
        t[size + 1] = 0;
    }

    bn_zero(out);
    for (int i = 0; i < size; i++) out->limb[i] = t[i];
    out->used = size;
    bn_trim(out);

    /* One conditional subtraction brings it into range. */
    if (t[size] || bn_compare(out, n) >= 0) bn_sub(out, n);
}

/* ------------------------------------------------------------------- RSA
 *
 * Verifying a signature is one modular exponentiation with the public
 * exponent, which is small and nearly always 65537 - seventeen bits with only
 * two of them set, so the whole thing is seventeen squarings and one
 * multiplication.
 */
bool rsa_public_op(const u8 *signature, size_t signature_len,
                   const u8 *modulus, size_t modulus_len,
                   u32 exponent, u8 *out, size_t out_len) {
    static bignum_t n, s, result, r2, base;

    if (!bn_from_bytes(&n, modulus, modulus_len)) return false;
    if (n.used == 0 || !(n.limb[0] & 1)) return false;      /* must be odd */
    if (!bn_from_bytes(&s, signature, signature_len)) return false;

    /* A signature at least as large as the modulus is not a signature. */
    if (bn_compare(&s, &n) >= 0) return false;

    u32 n0inv = mont_n0inv(n.limb[0]);

    /* The conversion constant, R^2 mod n, built by doubling from one.  R is
     * two to the power of the modulus's limb count times thirty-two, so this
     * is that many doublings twice over. */
    bn_zero(&r2);
    r2.limb[0] = 1;
    r2.used = 1;
    for (int i = 0; i < n.used * 32 * 2; i++) bn_double_mod(&r2, &n);

    /* Into the representation, and start from one. */
    mont_mul(&base, &s, &r2, &n, n0inv);

    bn_zero(&result);
    result.limb[0] = 1;
    result.used = 1;
    mont_mul(&result, &result, &r2, &n, n0inv);   /* one, in the representation */

    /* Square and multiply, from the top bit of the exponent down. */
    int top = 31;
    while (top > 0 && !((exponent >> top) & 1)) top--;
    for (int bit = top; bit >= 0; bit--) {
        mont_mul(&result, &result, &result, &n, n0inv);
        if ((exponent >> bit) & 1) mont_mul(&result, &result, &base, &n, n0inv);
    }

    /* And back out of it: multiplying by one in the representation is what
     * divides by R. */
    static bignum_t one;
    bn_zero(&one);
    one.limb[0] = 1;
    one.used = 1;
    mont_mul(&result, &result, &one, &n, n0inv);

    bn_to_bytes(&result, out, out_len);
    return true;
}

/* ------------------------------------------------------------------- test */

int bignum_selftest(void) {
    int failures = 0;

    /* A small case whose answer can be worked out by hand: 3^65537 mod 3233.
     * 3233 is 61 times 53, the modulus from every RSA worked example, and
     * 65537 mod lcm(60,52) leaves an exponent whose result is checkable. */
    {
        u8 modulus[2] = { 0x0c, 0xa1 };            /* 3233 */
        u8 base[2] = { 0x00, 0x03 };
        u8 out[2];
        /* 3^17 is 129140163, which leaves 1211 after 3233 goes into it 39944
         * times. */
        if (!rsa_public_op(base, sizeof base, modulus, sizeof modulus, 17,
                           out, sizeof out)) {
            kerr("bignum", "a small modular exponentiation was refused");
            failures++;
        } else {
            u32 value = ((u32)out[0] << 8) | out[1];
            if (value != 1211) {
                kerr("bignum", "3^17 mod 3233 came out as %u, expected 1211", value);
                failures++;
            }
        }
    }

    /* And the property that makes RSA work at all, on the textbook key:
     * n = 3233, e = 17, d = 413.  A message raised to e and then to d comes
     * back.  Doing it in both directions checks the arithmetic far more
     * thoroughly than any single product would. */
    {
        u8 modulus[2] = { 0x0c, 0xa1 };
        u8 message[2] = { 0x00, 0x41 };            /* 65 */
        u8 encrypted[2], decrypted[2];

        if (!rsa_public_op(message, sizeof message, modulus, sizeof modulus, 17,
                           encrypted, sizeof encrypted) ||
            !rsa_public_op(encrypted, sizeof encrypted, modulus, sizeof modulus, 413,
                           decrypted, sizeof decrypted)) {
            kerr("bignum", "the round trip was refused");
            failures++;
        } else if (((u32)encrypted[0] << 8 | encrypted[1]) != 2790) {
            kerr("bignum", "65 raised to 17 came out as %u, expected 2790",
                 (u32)encrypted[0] << 8 | encrypted[1]);
            failures++;
        } else {
            u32 back = ((u32)decrypted[0] << 8) | decrypted[1];
            if (back != 65) {
                kerr("bignum", "65 came back as %u after a round trip", back);
                failures++;
            }
        }
    }

    /* A signature that is not smaller than the modulus has to be refused: it
     * is not a valid signature and treating it as one would let a forged value
     * through the reduction. */
    {
        u8 modulus[2] = { 0x0c, 0xa1 };
        u8 too_big[2] = { 0x0c, 0xa1 };
        u8 out[2];
        if (rsa_public_op(too_big, sizeof too_big, modulus, sizeof modulus, 17,
                          out, sizeof out)) {
            kerr("bignum", "a signature as large as the modulus was accepted");
            failures++;
        }
    }

    if (!failures)
        kinfo("crypto", "the big-number arithmetic RSA needs is correct");
    return failures;
}
