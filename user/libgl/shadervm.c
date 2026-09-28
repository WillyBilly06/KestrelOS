/* shadervm.c - running a shader.
 *
 * The interpreter for the instruction set in shader.h.  It is deliberately
 * boring: every register is four floats, every instruction reads up to three
 * swizzled sources and writes a masked destination, and there are no cases in
 * the inner loop beyond the opcode itself.
 *
 * The write mask matters more than it looks.  A statement like
 *
 *     colour.rgb = colour.rgb * light;
 *
 * must leave alpha exactly as it was, and the only thing standing between that
 * and a shader that silently clears alpha to zero is that the destination is
 * masked rather than assigned whole.  The same goes for the swizzle on the
 * source side: reading .zyx has to be free, because source that reads that way
 * is ordinary rather than exotic.
 *
 * Everything else is arithmetic.  The one thing worth being careful about is
 * division and reciprocals: a shader that divides by zero on one pixel must
 * not produce an infinity that then contaminates the interpolation of a whole
 * triangle, so those are clamped to something finite and the picture stays
 * wrong only where it was already wrong.
 */
#include "shader.h"
#include "../libc/kestrel.h"
#include "../libc/math.h"

#define VERY_LARGE 1.0e30f

static inline void read_source(const sh_machine_t *m, uint16_t reg,
                               uint8_t swizzle, float *out) {
    const float *r = m->reg[reg];
    out[0] = r[(swizzle >> 0) & 3];
    out[1] = r[(swizzle >> 2) & 3];
    out[2] = r[(swizzle >> 4) & 3];
    out[3] = r[(swizzle >> 6) & 3];
}

static inline void write_dest(sh_machine_t *m, uint16_t reg, uint8_t mask,
                              const float *value) {
    float *r = m->reg[reg];
    if (mask & 1) r[0] = value[0];
    if (mask & 2) r[1] = value[1];
    if (mask & 4) r[2] = value[2];
    if (mask & 8) r[3] = value[3];
}

static inline float safe_rcp(float x) {
    if (x > -1.0e-20f && x < 1.0e-20f) return x < 0 ? -VERY_LARGE : VERY_LARGE;
    return 1.0f / x;
}

static inline float exp2f_(float x) { return powf(2.0f, x); }
static inline float log2f_(float x) {
    if (x <= 0) return -VERY_LARGE;
    return logf(x) * 1.4426950408889634f;
}

void sh_run(sh_machine_t *m, const sh_shader_t *shader) {
    if (!shader || !shader->compiled) return;

    m->discarded = false;
    m->executed = 0;

    int pc = 0;
    /* A ceiling on how long a shader may run.  Nothing in the source language
     * can produce an unbounded loop by accident, but a compiler bug could, and
     * a graphics driver that hangs the machine is worse than one that draws
     * the wrong colour. */
    int budget = 1 << 20;

    while (pc >= 0 && pc < shader->count && budget-- > 0) {
        const sh_instruction_t *in = &shader->code[pc];
        float a[4], b[4], c[4], out[4] = { 0, 0, 0, 0 };

        m->executed++;

        switch (in->op) {
        case SH_END:
            return;

        case SH_JMP:
            pc = in->target;
            continue;

        case SH_JMPZ:
            read_source(m, in->src[0], in->swizzle[0], a);
            if (a[0] == 0.0f) { pc = in->target; continue; }
            pc++;
            continue;

        case SH_JMPNZ:
            read_source(m, in->src[0], in->swizzle[0], a);
            if (a[0] != 0.0f) { pc = in->target; continue; }
            pc++;
            continue;

        case SH_DISCARD:
            m->discarded = true;
            return;

        case SH_TEX:
            read_source(m, in->src[0], in->swizzle[0], a);
            /* The unit is whatever the sampler's register holds, which is what
             * the application set with glUniform1i. */
            if (m->sample) m->sample((int)m->reg[in->src[1]][0], a[0], a[1], out);
            write_dest(m, in->dst, in->mask, out);
            pc++;
            continue;

        case SH_MATMUL: {
            /* Column major, the way OpenGL stores a matrix: the result's
             * component i is the dot of row i with the vector, and the rows
             * are spread one component per register. */
            read_source(m, in->src[1], in->swizzle[1], a);
            const float *c0 = m->reg[in->src[0] + 0];
            const float *c1 = m->reg[in->src[0] + 1];
            const float *c2 = m->reg[in->src[0] + 2];
            const float *c3 = m->reg[in->src[0] + 3];
            for (int i = 0; i < 4; i++)
                out[i] = c0[i] * a[0] + c1[i] * a[1] +
                         c2[i] * a[2] + c3[i] * a[3];
            write_dest(m, in->dst, in->mask, out);
            pc++;
            continue;
        }

        default:
            break;
        }

        read_source(m, in->src[0], in->swizzle[0], a);
        read_source(m, in->src[1], in->swizzle[1], b);
        read_source(m, in->src[2], in->swizzle[2], c);

        switch (in->op) {
        case SH_MOV:
            for (int i = 0; i < 4; i++) out[i] = a[i];
            break;
        case SH_ADD:
            for (int i = 0; i < 4; i++) out[i] = a[i] + b[i];
            break;
        case SH_SUB:
            for (int i = 0; i < 4; i++) out[i] = a[i] - b[i];
            break;
        case SH_MUL:
            for (int i = 0; i < 4; i++) out[i] = a[i] * b[i];
            break;
        case SH_DIV:
            for (int i = 0; i < 4; i++) out[i] = a[i] * safe_rcp(b[i]);
            break;
        case SH_MAD:
            for (int i = 0; i < 4; i++) out[i] = a[i] * b[i] + c[i];
            break;

        case SH_DP2: {
            float d = a[0] * b[0] + a[1] * b[1];
            for (int i = 0; i < 4; i++) out[i] = d;
            break;
        }
        case SH_DP3: {
            float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            for (int i = 0; i < 4; i++) out[i] = d;
            break;
        }
        case SH_DP4: {
            float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            for (int i = 0; i < 4; i++) out[i] = d;
            break;
        }

        case SH_MIN:
            for (int i = 0; i < 4; i++) out[i] = a[i] < b[i] ? a[i] : b[i];
            break;
        case SH_MAX:
            for (int i = 0; i < 4; i++) out[i] = a[i] > b[i] ? a[i] : b[i];
            break;

        case SH_CROSS:
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
            out[3] = 0.0f;
            break;

        case SH_RCP:
            for (int i = 0; i < 4; i++) out[i] = safe_rcp(a[i]);
            break;
        case SH_RSQ:
            for (int i = 0; i < 4; i++)
                out[i] = a[i] > 0 ? 1.0f / sqrtf(a[i]) : VERY_LARGE;
            break;
        case SH_SQRT:
            for (int i = 0; i < 4; i++) out[i] = a[i] > 0 ? sqrtf(a[i]) : 0.0f;
            break;
        case SH_ABS:
            for (int i = 0; i < 4; i++) out[i] = fabsf(a[i]);
            break;
        case SH_NEG:
            for (int i = 0; i < 4; i++) out[i] = -a[i];
            break;
        case SH_FLOOR:
            for (int i = 0; i < 4; i++) out[i] = floorf(a[i]);
            break;
        case SH_FRACT:
            for (int i = 0; i < 4; i++) out[i] = a[i] - floorf(a[i]);
            break;
        case SH_SIGN:
            for (int i = 0; i < 4; i++)
                out[i] = a[i] > 0 ? 1.0f : (a[i] < 0 ? -1.0f : 0.0f);
            break;
        case SH_SIN:
            for (int i = 0; i < 4; i++) out[i] = sinf(a[i]);
            break;
        case SH_COS:
            for (int i = 0; i < 4; i++) out[i] = cosf(a[i]);
            break;
        case SH_POW:
            for (int i = 0; i < 4; i++)
                out[i] = a[i] > 0 ? powf(a[i], b[i]) : 0.0f;
            break;
        case SH_EXP2:
            for (int i = 0; i < 4; i++) out[i] = exp2f_(a[i]);
            break;
        case SH_LOG2:
            for (int i = 0; i < 4; i++) out[i] = log2f_(a[i]);
            break;

        /* The comparisons produce one or zero rather than branching, because
         * that is what lets a shader select between two values without a jump
         * - and a jump per pixel is what a rasteriser cannot afford. */
        case SH_SLT:
            for (int i = 0; i < 4; i++) out[i] = a[i] <  b[i] ? 1.0f : 0.0f;
            break;
        case SH_SLE:
            for (int i = 0; i < 4; i++) out[i] = a[i] <= b[i] ? 1.0f : 0.0f;
            break;
        case SH_SGT:
            for (int i = 0; i < 4; i++) out[i] = a[i] >  b[i] ? 1.0f : 0.0f;
            break;
        case SH_SGE:
            for (int i = 0; i < 4; i++) out[i] = a[i] >= b[i] ? 1.0f : 0.0f;
            break;
        case SH_SEQ:
            for (int i = 0; i < 4; i++) out[i] = a[i] == b[i] ? 1.0f : 0.0f;
            break;
        case SH_SNE:
            for (int i = 0; i < 4; i++) out[i] = a[i] != b[i] ? 1.0f : 0.0f;
            break;

        case SH_AND:
            for (int i = 0; i < 4; i++)
                out[i] = (a[i] != 0.0f && b[i] != 0.0f) ? 1.0f : 0.0f;
            break;
        case SH_OR:
            for (int i = 0; i < 4; i++)
                out[i] = (a[i] != 0.0f || b[i] != 0.0f) ? 1.0f : 0.0f;
            break;
        case SH_NOT:
            for (int i = 0; i < 4; i++) out[i] = a[i] == 0.0f ? 1.0f : 0.0f;
            break;

        default:
            break;
        }

        write_dest(m, in->dst, in->mask, out);
        pc++;
    }
}

/* --------------------------------------------------------------- locations */

int sh_uniform_location(const sh_program_t *p, const char *name) {
    if (!p || !p->linked || !name) return -1;
    for (int i = 0; i < p->nuniforms; i++)
        if (!strcmp(p->uniform[i].name, name)) return i;
    return -1;
}

int sh_attribute_location(const sh_program_t *p, const char *name) {
    if (!p || !p->linked || !name) return -1;
    for (int i = 0; i < p->nattributes; i++)
        if (!strcmp(p->attribute[i].name, name)) return i;
    return -1;
}
