/* x25519.c - the key agreement every modern connection starts with.
 *
 * Two machines that have never met need a shared secret over a wire anyone can
 * listen to.  Each picks a random number, multiplies a fixed point on a curve
 * by it, and sends the result; each then multiplies what it received by its own
 * number.  Both arrive at the same point, and an eavesdropper who saw both
 * public halves cannot work out either private one.
 *
 * The curve is the one Bernstein designed for exactly this, and it is the
 * default in TLS 1.3 because it is fast, has no parameter choices to get wrong,
 * and needs no conditional branches on secret data - which is what makes a
 * straightforward implementation also a safe one.
 *
 * Everything below works in a representation where a number is ten limbs of
 * about twenty-six bits each.  Twenty-six rather than thirty-two so that
 * several can be added or multiplied together before anything overflows, which
 * is what lets the carries be resolved once at the end rather than after every
 * operation.
 */
#include "kernel.h"
#include "crypto.h"

/* A field element: ten limbs, alternating twenty-six and twenty-five bits, so
 * that ten of them cover the 255 bits the curve needs. */
typedef s64 fe[10];

static void fe_zero(fe out) { for (int i = 0; i < 10; i++) out[i] = 0; }
static void fe_one(fe out) { fe_zero(out); out[0] = 1; }
static void fe_copy(fe out, const fe in) { for (int i = 0; i < 10; i++) out[i] = in[i]; }

static void fe_add(fe out, const fe a, const fe b) {
    for (int i = 0; i < 10; i++) out[i] = a[i] + b[i];
}
static void fe_sub(fe out, const fe a, const fe b) {
    for (int i = 0; i < 10; i++) out[i] = a[i] - b[i];
}

/* Reduce a product back into ten limbs.  The curve's modulus is 2^255 - 19, so
 * anything that overflows the top limb comes back in at the bottom multiplied
 * by nineteen - which is the whole reason this modulus was chosen. */
static void fe_carry(fe h) {
    s64 c;
    for (int i = 0; i < 10; i++) {
        int shift = (i & 1) ? 25 : 26;
        c = (h[i] + (1LL << (shift - 1))) >> shift;
        if (i == 9) h[0] += c * 19;
        else h[i + 1] += c;
        h[i] -= c << shift;
    }
}

/* Multiplication.  Every limb of one meets every limb of the other; the ones
 * that land past the top come back multiplied by nineteen, and the alternating
 * limb sizes mean half of those cross-terms need doubling as well. */
static void fe_mul(fe out, const fe f, const fe g) {
    s64 g1_19 = 19 * g[1], g2_19 = 19 * g[2], g3_19 = 19 * g[3];
    s64 g4_19 = 19 * g[4], g5_19 = 19 * g[5], g6_19 = 19 * g[6];
    s64 g7_19 = 19 * g[7], g8_19 = 19 * g[8], g9_19 = 19 * g[9];
    s64 f1_2 = 2 * f[1], f3_2 = 2 * f[3], f5_2 = 2 * f[5];
    s64 f7_2 = 2 * f[7], f9_2 = 2 * f[9];

    s64 h[10];
    h[0] = f[0]*g[0] + f1_2*g9_19 + f[2]*g8_19 + f3_2*g7_19 + f[4]*g6_19
         + f5_2*g5_19 + f[6]*g4_19 + f7_2*g3_19 + f[8]*g2_19 + f9_2*g1_19;
    h[1] = f[0]*g[1] + f[1]*g[0] + f[2]*g9_19 + f[3]*g8_19 + f[4]*g7_19
         + f[5]*g6_19 + f[6]*g5_19 + f[7]*g4_19 + f[8]*g3_19 + f[9]*g2_19;
    h[2] = f[0]*g[2] + f1_2*g[1] + f[2]*g[0] + f3_2*g9_19 + f[4]*g8_19
         + f5_2*g7_19 + f[6]*g6_19 + f7_2*g5_19 + f[8]*g4_19 + f9_2*g3_19;
    h[3] = f[0]*g[3] + f[1]*g[2] + f[2]*g[1] + f[3]*g[0] + f[4]*g9_19
         + f[5]*g8_19 + f[6]*g7_19 + f[7]*g6_19 + f[8]*g5_19 + f[9]*g4_19;
    h[4] = f[0]*g[4] + f1_2*g[3] + f[2]*g[2] + f3_2*g[1] + f[4]*g[0]
         + f5_2*g9_19 + f[6]*g8_19 + f7_2*g7_19 + f[8]*g6_19 + f9_2*g5_19;
    h[5] = f[0]*g[5] + f[1]*g[4] + f[2]*g[3] + f[3]*g[2] + f[4]*g[1]
         + f[5]*g[0] + f[6]*g9_19 + f[7]*g8_19 + f[8]*g7_19 + f[9]*g6_19;
    h[6] = f[0]*g[6] + f1_2*g[5] + f[2]*g[4] + f3_2*g[3] + f[4]*g[2]
         + f5_2*g[1] + f[6]*g[0] + f7_2*g9_19 + f[8]*g8_19 + f9_2*g7_19;
    h[7] = f[0]*g[7] + f[1]*g[6] + f[2]*g[5] + f[3]*g[4] + f[4]*g[3]
         + f[5]*g[2] + f[6]*g[1] + f[7]*g[0] + f[8]*g9_19 + f[9]*g8_19;
    h[8] = f[0]*g[8] + f1_2*g[7] + f[2]*g[6] + f3_2*g[5] + f[4]*g[4]
         + f5_2*g[3] + f[6]*g[2] + f7_2*g[1] + f[8]*g[0] + f9_2*g9_19;
    h[9] = f[0]*g[9] + f[1]*g[8] + f[2]*g[7] + f[3]*g[6] + f[4]*g[5]
         + f[5]*g[4] + f[6]*g[3] + f[7]*g[2] + f[8]*g[1] + f[9]*g[0];

    for (int i = 0; i < 10; i++) out[i] = h[i];
    fe_carry(out);
    fe_carry(out);
}

static void fe_sq(fe out, const fe f) { fe_mul(out, f, f); }

/* Multiply by a small constant, which the ladder needs once per step. */
static void fe_mul121666(fe out, const fe f) {
    for (int i = 0; i < 10; i++) out[i] = f[i] * 121666;
    fe_carry(out);
    fe_carry(out);
}

/* The inverse, by raising to the power p - 2.  Fermat's little theorem says
 * that is the inverse, and doing it as a fixed chain of squarings means the
 * work does not depend on the value - which matters when the value is secret. */
static void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;
    int i;

    fe_sq(t0, z);
    fe_sq(t1, t0); fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);
    fe_mul(t1, t1, t2);
    fe_sq(t2, t1);
    for (i = 1; i < 5; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (i = 1; i < 20; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (i = 1; i < 100; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (i = 1; i < 5; i++) fe_sq(t1, t1);
    fe_mul(out, t1, t0);
}

/* Swap two elements if the flag is set, without branching on the flag: the
 * flag becomes a mask of all ones or all zeros and the exchange is arithmetic.
 * A branch here would leak which way the secret bit went through the timing. */
static void fe_cswap(fe f, fe g, u32 flag) {
    s64 mask = -(s64)flag;
    for (int i = 0; i < 10; i++) {
        s64 x = mask & (f[i] ^ g[i]);
        f[i] ^= x;
        g[i] ^= x;
    }
}

/* Reading and writing the thirty-two byte form that goes on the wire. */
static void fe_from_bytes(fe out, const u8 in[32]) {
    s64 h[10];
    #define LOAD(n, count) ({ u64 v = 0; for (int k = 0; k < (count); k++) \
        v |= (u64)in[(n) + k] << (8 * k); (s64)v; })

    h[0] = LOAD(0, 4) & 0x3FFFFFF;
    h[1] = (LOAD(3, 4) >> 2) & 0x1FFFFFF;
    h[2] = (LOAD(6, 4) >> 3) & 0x3FFFFFF;
    h[3] = (LOAD(9, 4) >> 5) & 0x1FFFFFF;
    h[4] = (LOAD(12, 4) >> 6) & 0x3FFFFFF;
    h[5] = LOAD(16, 4) & 0x1FFFFFF;
    h[6] = (LOAD(19, 4) >> 1) & 0x3FFFFFF;
    h[7] = (LOAD(22, 4) >> 3) & 0x1FFFFFF;
    h[8] = (LOAD(25, 4) >> 4) & 0x3FFFFFF;
    h[9] = (LOAD(28, 4) >> 6) & 0x1FFFFFF;
    #undef LOAD

    for (int i = 0; i < 10; i++) out[i] = h[i];
}

static void fe_to_bytes(u8 out[32], const fe in) {
    fe h;
    fe_copy(h, in);
    fe_carry(h);
    fe_carry(h);
    fe_carry(h);

    /* Bring every limb into range, and subtract the modulus once if the value
     * is still at or above it. */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 10; i++) {
            int shift = (i & 1) ? 25 : 26;
            s64 c = h[i] >> shift;
            if (i == 9) h[0] += c * 19;
            else h[i + 1] += c;
            h[i] -= c << shift;
        }
    }

    /* q is 1 when the value is at least the modulus. */
    s64 q = (h[9] >> 25) & 1;
    q = (h[0] + 19 * q) >> 26;
    for (int i = 1; i < 10; i++) q = (h[i] + q) >> ((i & 1) ? 25 : 26);

    h[0] += 19 * q;
    for (int i = 0; i < 9; i++) {
        int shift = (i & 1) ? 25 : 26;
        s64 c = h[i] >> shift;
        h[i + 1] += c;
        h[i] -= c << shift;
    }
    h[9] &= 0x1FFFFFF;

    /* Pack the limbs back into bytes. */
    u64 acc = 0;
    int bits = 0, at = 0;
    for (int i = 0; i < 10; i++) {
        int size = (i & 1) ? 25 : 26;
        acc |= ((u64)(h[i] & ((1LL << size) - 1))) << bits;
        bits += size;
        while (bits >= 8 && at < 32) {
            out[at++] = (u8)acc;
            acc >>= 8;
            bits -= 8;
        }
    }
    while (at < 32) out[at++] = (u8)acc, acc >>= 8;
}

/* ------------------------------------------------------------- the ladder
 *
 * The multiplication itself.  For each bit of the scalar, from the top down,
 * two candidate points are advanced together and swapped according to the bit.
 * Both branches do the same work whichever way the bit goes, so the time taken
 * says nothing about the secret.
 */
void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]) {
    u8 e[32];
    memcpy(e, scalar, 32);

    /* The scalar is clamped: the low three bits cleared so it is a multiple of
     * the cofactor, the top bit cleared and the next set so it is always the
     * same length.  Both are what stop a whole family of attacks. */
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    fe_from_bytes(x1, point);
    fe_one(x2);
    fe_zero(z2);
    fe_copy(x3, x1);
    fe_one(z3);

    u32 swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        u32 bit = (e[pos / 8] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2, x2, z2);
        fe_add(z2, x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2, tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        fe_mul121666(z3, tmp1);
        fe_sq(x3, x3);
        fe_add(tmp0, tmp0, z3);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_to_bytes(out, x2);
}

/* The public half of a private key: the curve's base point multiplied by it.
 * The base point is the number nine. */
void x25519_public(u8 out[32], const u8 secret[32]) {
    static const u8 base[32] = { 9 };
    x25519(out, secret, base);
}

/* ------------------------------------------------------------------- test */

/* The vectors RFC 7748 publishes.  A curve implementation that is subtly wrong
 * still produces plausible-looking output and a connection that fails much
 * later for no visible reason, so these are checked before anything uses it. */
int x25519_selftest(void) {
    static const u8 alice_secret[32] = {
        0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
        0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a,
    };
    static const u8 alice_public[32] = {
        0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
        0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a,
    };
    static const u8 bob_secret[32] = {
        0x5d,0xab,0x08,0x7e,0x62,0x4a,0x8a,0x4b,0x79,0xe1,0x7f,0x8b,0x83,0x80,0x0e,0xe6,
        0x6f,0x3b,0xb1,0x29,0x26,0x18,0xb6,0xfd,0x1c,0x2f,0x8b,0x27,0xff,0x88,0xe0,0xeb,
    };
    static const u8 bob_public[32] = {
        0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,
        0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f,
    };
    static const u8 shared[32] = {
        0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,
        0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42,
    };

    int failures = 0;
    u8 out[32];

    x25519_public(out, alice_secret);
    if (memcmp(out, alice_public, 32)) {
        kerr("x25519", "the public half of a known key came out wrong");
        failures++;
    }
    x25519_public(out, bob_secret);
    if (memcmp(out, bob_public, 32)) {
        kerr("x25519", "the second public half came out wrong");
        failures++;
    }

    /* And the point of the whole thing: both sides reach the same secret. */
    x25519(out, alice_secret, bob_public);
    if (memcmp(out, shared, 32)) {
        kerr("x25519", "the shared secret came out wrong from one side");
        failures++;
    }
    x25519(out, bob_secret, alice_public);
    if (memcmp(out, shared, 32)) {
        kerr("x25519", "the shared secret came out wrong from the other side");
        failures++;
    }

    if (!failures)
        kinfo("crypto", "x25519 matches the vectors RFC 7748 publishes");
    return failures;
}
