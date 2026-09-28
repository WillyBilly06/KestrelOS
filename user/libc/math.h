/* math.h - the floating-point routines a renderer needs.
 *
 * Small and exact enough for graphics: the trigonometric functions are
 * minimax polynomials over a reduced range, which is what a rasteriser wants
 * from them - a few million calls a second and an error far below one pixel.
 */
#ifndef KESTREL_MATH_H
#define KESTREL_MATH_H

#define M_PI    3.14159265358979323846
#define M_PI_2  1.57079632679489661923
#define M_TAU   6.28318530717958647693

float  sqrtf(float x);
float  fabsf(float x);
float  floorf(float x);
float  ceilf(float x);
float  fmodf(float x, float y);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  asinf(float x);
float  acosf(float x);
float  atanf(float x);
float  atan2f(float y, float x);
float  expf(float x);
float  logf(float x);
float  powf(float x, float y);

double sqrt(double x);
double fabs(double x);
double floor(double x);
double sin(double x);
double cos(double x);
double pow(double x, double y);

static inline float radiansf(float degrees) { return degrees * (float)(M_PI / 180.0); }
static inline float degreesf(float radians) { return radians * (float)(180.0 / M_PI); }

#endif
