/* math.c - floating point for a renderer.
 *
 * Square root is a single instruction on this processor.  The rest are minimax
 * polynomials over a reduced range: a rasteriser calls these millions of times
 * a second and needs an error well under a pixel, not the last bit of a double.
 */
#include "math.h"
#include <stdint.h>

/* ------------------------------------------------------------------ basics */

float sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

float  fabsf(float x) { return x < 0.0f ? -x : x; }
double fabs(double x) { return x < 0.0 ? -x : x; }

float floorf(float x) {
    float t = (float)(long long)x;
    return (x < 0.0f && t != x) ? t - 1.0f : t;
}

double floor(double x) {
    double t = (double)(long long)x;
    return (x < 0.0 && t != x) ? t - 1.0 : t;
}

float ceilf(float x) {
    float t = (float)(long long)x;
    return (x > 0.0f && t != x) ? t + 1.0f : t;
}

float fmodf(float x, float y) {
    if (y == 0.0f) return 0.0f;
    float q = x / y;
    return x - y * (float)(long long)q;
}

/* ------------------------------------------------------------ trigonometry */

/* Reduce to [-pi, pi], where the polynomial below is accurate, then evaluate.
 * The coefficients are the odd terms of the Taylor series adjusted so the
 * error is spread across the interval rather than piling up at the ends. */
static float sin_reduced(float x) {
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f
                    + x2 * (0.008333331f
                    + x2 * (-0.00019840874f
                    + x2 * (2.7525562e-6f
                    + x2 * -2.5050760e-8f)))));
}

static float wrap_pi(float x) {
    /* Subtract whole turns first so a large angle does not lose every digit. */
    const float tau = (float)M_TAU;
    if (x < -1e9f || x > 1e9f) return 0.0f;
    float turns = x * (1.0f / (float)M_TAU);
    turns = turns - floorf(turns);          /* [0, 1) */
    x = turns * tau;                        /* [0, tau) */
    if (x > (float)M_PI) x -= tau;          /* (-pi, pi] */
    return x;
}

float sinf(float x) { return sin_reduced(wrap_pi(x)); }

float cosf(float x) {
    /* cos is sin a quarter turn along; reducing after the shift keeps the
     * argument in the range the polynomial is fitted over. */
    return sin_reduced(wrap_pi(x + (float)M_PI_2));
}

float tanf(float x) {
    float c = cosf(x);
    if (fabsf(c) < 1e-7f) return x > 0.0f ? 1e30f : -1e30f;
    return sinf(x) / c;
}

double sin(double x) { return (double)sinf((float)x); }
double cos(double x) { return (double)cosf((float)x); }

/* Minimax fit for atan over [-1, 1]; anything outside is folded in by the
 * identity atan(x) = pi/2 - atan(1/x). */
static float atan_unit(float x) {
    float x2 = x * x;
    return x * (0.99997726f
         + x2 * (-0.33262347f
         + x2 * (0.19354346f
         + x2 * (-0.11643287f
         + x2 * (0.05265332f
         + x2 * -0.01172120f)))));
}

float atanf(float x) {
    if (fabsf(x) <= 1.0f) return atan_unit(x);
    float r = (float)M_PI_2 - atan_unit(1.0f / fabsf(x));
    return x < 0.0f ? -r : r;
}

float atan2f(float y, float x) {
    if (x > 0.0f) return atanf(y / x);
    if (x < 0.0f) return atanf(y / x) + (y >= 0.0f ? (float)M_PI : -(float)M_PI);
    if (y > 0.0f) return (float)M_PI_2;
    if (y < 0.0f) return -(float)M_PI_2;
    return 0.0f;
}

float asinf(float x) {
    if (x <= -1.0f) return -(float)M_PI_2;
    if (x >=  1.0f) return  (float)M_PI_2;
    return atanf(x / sqrtf(1.0f - x * x));
}

float acosf(float x) { return (float)M_PI_2 - asinf(x); }

/* ------------------------------------------------------- exp, log and power */

/* Split into an integer power of two and a remainder: the exponent is set
 * directly in the result's bits and only the remainder needs a polynomial. */
float expf(float x) {
    if (x < -87.0f) return 0.0f;
    if (x >  88.0f) return 3.4e38f;

    const float log2e = 1.44269504f;
    float t = x * log2e;
    int   n = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
    float f = x - (float)n * 0.69314718f;      /* remainder in ln terms */

    float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f
                     + f * (0.04166667f + f * 0.00833333f))));

    union { float f; uint32_t u; } scale;
    int exponent = n + 127;
    if (exponent <= 0)   return 0.0f;
    if (exponent >= 255) return 3.4e38f;
    scale.u = (uint32_t)exponent << 23;
    return p * scale.f;
}

float logf(float x) {
    if (x <= 0.0f) return -1e30f;

    union { float f; uint32_t u; } v = { x };
    int   exponent = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;   /* mantissa into [1, 2) */
    float m = v.f;

    /* atanh form: converges quickly across the whole octave. */
    float s = (m - 1.0f) / (m + 1.0f);
    float s2 = s * s;
    float ln_m = 2.0f * s * (1.0f + s2 * (0.33333333f
                          + s2 * (0.2f + s2 * (0.14285714f + s2 * 0.11111111f))));

    return ln_m + (float)exponent * 0.69314718f;
}

float powf(float x, float y) {
    if (y == 0.0f) return 1.0f;
    if (x == 0.0f) return 0.0f;

    if (x < 0.0f) {
        /* Only an integer exponent is defined; the sign comes from parity. */
        float ry = y - floorf(y);
        if (ry != 0.0f) return 0.0f;
        float r = expf(y * logf(-x));
        return ((long long)y & 1) ? -r : r;
    }
    return expf(y * logf(x));
}

double pow(double x, double y) { return (double)powf((float)x, (float)y); }
