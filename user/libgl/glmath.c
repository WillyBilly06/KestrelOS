/* glmath.c - 4x4 matrices, laid out the way OpenGL expects them.
 *
 * Column major: element (row, column) lives at m[column * 4 + row].  That is
 * the layout glLoadMatrixf and glMultMatrixf take, so keeping it internally
 * means a matrix handed in by a program needs no rearranging.
 */
#include "glstate.h"
#include "../libc/math.h"

#define AT(m, row, col) ((m)[(col) * 4 + (row)])

mat4_t mat4_identity(void) {
    mat4_t r;
    for (int i = 0; i < 16; i++) r.m[i] = 0.0f;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

mat4_t mat4_multiply(const mat4_t *a, const mat4_t *b) {
    mat4_t r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += AT(a->m, row, k) * AT(b->m, k, col);
            AT(r.m, row, col) = sum;
        }
    }
    return r;
}

vec4_t mat4_transform(const mat4_t *m, vec4_t v) {
    vec4_t r;
    r.x = AT(m->m,0,0)*v.x + AT(m->m,0,1)*v.y + AT(m->m,0,2)*v.z + AT(m->m,0,3)*v.w;
    r.y = AT(m->m,1,0)*v.x + AT(m->m,1,1)*v.y + AT(m->m,1,2)*v.z + AT(m->m,1,3)*v.w;
    r.z = AT(m->m,2,0)*v.x + AT(m->m,2,1)*v.y + AT(m->m,2,2)*v.z + AT(m->m,2,3)*v.w;
    r.w = AT(m->m,3,0)*v.x + AT(m->m,3,1)*v.y + AT(m->m,3,2)*v.z + AT(m->m,3,3)*v.w;
    return r;
}

mat4_t mat4_translate(float x, float y, float z) {
    mat4_t r = mat4_identity();
    AT(r.m, 0, 3) = x;
    AT(r.m, 1, 3) = y;
    AT(r.m, 2, 3) = z;
    return r;
}

mat4_t mat4_scale(float x, float y, float z) {
    mat4_t r = mat4_identity();
    AT(r.m, 0, 0) = x;
    AT(r.m, 1, 1) = y;
    AT(r.m, 2, 2) = z;
    return r;
}

mat4_t mat4_rotate(float radians, float x, float y, float z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len < 1e-8f) return mat4_identity();
    x /= len; y /= len; z /= len;

    float c = cosf(radians), s = sinf(radians), t = 1.0f - c;
    mat4_t r = mat4_identity();

    AT(r.m,0,0) = t*x*x + c;    AT(r.m,0,1) = t*x*y - s*z;  AT(r.m,0,2) = t*x*z + s*y;
    AT(r.m,1,0) = t*x*y + s*z;  AT(r.m,1,1) = t*y*y + c;    AT(r.m,1,2) = t*y*z - s*x;
    AT(r.m,2,0) = t*x*z - s*y;  AT(r.m,2,1) = t*y*z + s*x;  AT(r.m,2,2) = t*z*z + c;
    return r;
}

mat4_t mat4_ortho(float l, float r, float b, float t, float n, float f) {
    mat4_t o = mat4_identity();
    if (r == l || t == b || f == n) return o;

    AT(o.m,0,0) = 2.0f / (r - l);
    AT(o.m,1,1) = 2.0f / (t - b);
    AT(o.m,2,2) = -2.0f / (f - n);
    AT(o.m,0,3) = -(r + l) / (r - l);
    AT(o.m,1,3) = -(t + b) / (t - b);
    AT(o.m,2,3) = -(f + n) / (f - n);
    return o;
}

mat4_t mat4_frustum(float l, float r, float b, float t, float n, float f) {
    mat4_t p;
    for (int i = 0; i < 16; i++) p.m[i] = 0.0f;
    if (r == l || t == b || f == n || n <= 0.0f) return mat4_identity();

    AT(p.m,0,0) = 2.0f * n / (r - l);
    AT(p.m,1,1) = 2.0f * n / (t - b);
    AT(p.m,0,2) = (r + l) / (r - l);
    AT(p.m,1,2) = (t + b) / (t - b);
    AT(p.m,2,2) = -(f + n) / (f - n);
    AT(p.m,2,3) = -2.0f * f * n / (f - n);
    AT(p.m,3,2) = -1.0f;
    return p;
}

/* A normal is not transformed by the model matrix but by the inverse transpose
 * of its upper 3x3: under a non-uniform scale the surface tilts one way and its
 * normal the other, and using the model matrix directly would light the object
 * as though it had never been squashed. */
mat4_t mat4_normal_matrix(const mat4_t *mv) {
    const float *m = mv->m;

    float a = AT(m,0,0), b = AT(m,0,1), c = AT(m,0,2);
    float d = AT(m,1,0), e = AT(m,1,1), f = AT(m,1,2);
    float g = AT(m,2,0), h = AT(m,2,1), i = AT(m,2,2);

    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    mat4_t r = mat4_identity();
    if (fabsf(det) < 1e-12f) {
        /* Degenerate: fall back to the rotation part, which is right whenever
         * the matrix is a rotation and no worse than nothing when it is not. */
        AT(r.m,0,0)=a; AT(r.m,0,1)=b; AT(r.m,0,2)=c;
        AT(r.m,1,0)=d; AT(r.m,1,1)=e; AT(r.m,1,2)=f;
        AT(r.m,2,0)=g; AT(r.m,2,1)=h; AT(r.m,2,2)=i;
        return r;
    }
    float inv = 1.0f / det;

    /* The transpose of the adjugate, which is the inverse transpose. */
    AT(r.m,0,0) =  (e * i - f * h) * inv;
    AT(r.m,1,0) = -(b * i - c * h) * inv;
    AT(r.m,2,0) =  (b * f - c * e) * inv;
    AT(r.m,0,1) = -(d * i - f * g) * inv;
    AT(r.m,1,1) =  (a * i - c * g) * inv;
    AT(r.m,2,1) = -(a * f - c * d) * inv;
    AT(r.m,0,2) =  (d * h - e * g) * inv;
    AT(r.m,1,2) = -(a * h - b * g) * inv;
    AT(r.m,2,2) =  (a * e - b * d) * inv;
    return r;
}
