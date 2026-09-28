/* glsl.c - turning shader source into something that runs.
 *
 * A compiler for the subset of GLSL that shaders are actually written in:
 * uniforms, attributes and varyings; float, vec2, vec3, vec4, mat4 and
 * sampler2D; the arithmetic and comparison operators; swizzles and write
 * masks; if and for; and the builtin functions everything uses - dot, cross,
 * normalize, mix, clamp, pow, texture2D and the rest.
 *
 * It is a single pass: a recursive-descent parser that emits instructions as
 * it goes into temporary registers, with no intermediate tree.  That is enough
 * because the source language has no forward references inside a function and
 * no aliasing to reason about, and it keeps the whole thing small enough to
 * read.
 *
 * Three parts of it are where the real work is, and each is a place where a
 * shading language quietly differs from an ordinary one:
 *
 *   Swizzles compose.  Writing v.zyx.yz is legal and means the y and z of the
 *   already-reversed vector, so a swizzle is applied to whatever swizzle the
 *   value already carried rather than replacing it.
 *
 *   Assignment through a swizzle is a permutation, not a copy.  In
 *   "v.yx = w" the y component takes w.x and the x component takes w.y.  A
 *   compiler that builds only a write mask and forgets to permute the source
 *   produces a shader that is right whenever the swizzle happens to be in
 *   order and wrong the rest of the time - which is the worst kind of wrong,
 *   because most shaders are mostly in order.
 *
 *   Scalars broadcast.  "v * 2.0" multiplies every component, and the way to
 *   do that without a special instruction is to read the scalar through the
 *   swizzle .xxxx.  Once that is in place a great deal of the type system
 *   disappears.
 */
#include "shader.h"
#include "../libc/kestrel.h"
#include "../libc/math.h"

/* ------------------------------------------------------------------ tokens */

typedef enum { T_END, T_IDENT, T_NUMBER, T_PUNCT } tok_kind_t;

typedef struct {
    tok_kind_t kind;
    char       text[SH_NAME_MAX];
    float      number;
} token_t;

typedef struct {
    uint16_t reg;
    uint8_t  swizzle;
    int      comps;          /* 1 to 4 */
    bool     is_matrix;      /* reg is the first of four                    */
    bool     is_sampler;
    int      sampler_unit;
    bool     lvalue;
} value_t;

#define MAX_LOCALS 64

typedef struct {
    char     name[SH_NAME_MAX];
    uint16_t reg;
    int      comps;
    bool     is_matrix;
    bool     is_sampler;
    int      sampler_unit;
} symbol_t;

typedef struct {
    const char  *src;
    int          at;
    token_t      tok;
    token_t      ahead;
    bool         have_ahead;

    sh_shader_t *sh;
    bool         failed;

    symbol_t     local[MAX_LOCALS];
    int          nlocals;
    int          scope[16];
    int          depth;

    int          local_top;   /* first temporary register                   */
    int          temp_top;    /* next free temporary                        */
    int          high_water;

    int          samplers;
    int          next_uniform;
    int          next_attrib;
    int          next_varying;
} compiler_t;

static void fail(compiler_t *g, const char *what) {
    if (g->failed) return;
    g->failed = true;
    snprintf(g->sh->log, sizeof g->sh->log, "%s (near \"%s\")", what,
             g->tok.text[0] ? g->tok.text : "end of source");
}

/* ------------------------------------------------------------------ lexing */

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static void lex(compiler_t *g, token_t *out) {
    const char *s = g->src;

    for (;;) {
        while (s[g->at] == ' ' || s[g->at] == '\t' || s[g->at] == '\n' ||
               s[g->at] == '\r')
            g->at++;

        /* Preprocessor lines are skipped whole: a version directive and a
         * handful of #defines are all real shaders carry, and honouring them
         * is not what makes the picture right. */
        if (s[g->at] == '#') {
            while (s[g->at] && s[g->at] != '\n') g->at++;
            continue;
        }
        if (s[g->at] == '/' && s[g->at + 1] == '/') {
            while (s[g->at] && s[g->at] != '\n') g->at++;
            continue;
        }
        if (s[g->at] == '/' && s[g->at + 1] == '*') {
            g->at += 2;
            while (s[g->at] && !(s[g->at] == '*' && s[g->at + 1] == '/')) g->at++;
            if (s[g->at]) g->at += 2;
            continue;
        }
        break;
    }

    memset(out, 0, sizeof *out);

    if (!s[g->at]) { out->kind = T_END; return; }

    if (is_alpha(s[g->at])) {
        int n = 0;
        while ((is_alpha(s[g->at]) || is_digit(s[g->at])) && n < SH_NAME_MAX - 1)
            out->text[n++] = s[g->at++];
        while (is_alpha(s[g->at]) || is_digit(s[g->at])) g->at++;
        out->text[n] = 0;
        out->kind = T_IDENT;
        return;
    }

    if (is_digit(s[g->at]) || (s[g->at] == '.' && is_digit(s[g->at + 1]))) {
        float whole = 0;
        while (is_digit(s[g->at])) whole = whole * 10 + (s[g->at++] - '0');
        if (s[g->at] == '.') {
            g->at++;
            float scale = 0.1f;
            while (is_digit(s[g->at])) {
                whole += (s[g->at++] - '0') * scale;
                scale *= 0.1f;
            }
        }
        if (s[g->at] == 'e' || s[g->at] == 'E') {
            g->at++;
            int sign = 1;
            if (s[g->at] == '-') { sign = -1; g->at++; }
            else if (s[g->at] == '+') g->at++;
            int e = 0;
            while (is_digit(s[g->at])) e = e * 10 + (s[g->at++] - '0');
            whole *= powf(10.0f, (float)(sign * e));
        }
        if (s[g->at] == 'f' || s[g->at] == 'F') g->at++;
        out->kind = T_NUMBER;
        out->number = whole;
        return;
    }

    /* Two-character operators have to be tried before one-character ones, or
     * "<=" lexes as "<" and the shader means something else. */
    static const char *two[] = { "<=", ">=", "==", "!=", "&&", "||", "+=",
                                 "-=", "*=", "/=", "++", "--", NULL };
    for (int i = 0; two[i]; i++) {
        if (s[g->at] == two[i][0] && s[g->at + 1] == two[i][1]) {
            out->kind = T_PUNCT;
            out->text[0] = two[i][0];
            out->text[1] = two[i][1];
            g->at += 2;
            return;
        }
    }

    out->kind = T_PUNCT;
    out->text[0] = s[g->at++];
}

static void advance(compiler_t *g) {
    if (g->have_ahead) { g->tok = g->ahead; g->have_ahead = false; return; }
    lex(g, &g->tok);
}

static const token_t *peek(compiler_t *g) {
    if (!g->have_ahead) { lex(g, &g->ahead); g->have_ahead = true; }
    return &g->ahead;
}

static bool is_punct(compiler_t *g, const char *p) {
    return g->tok.kind == T_PUNCT && !strcmp(g->tok.text, p);
}
static bool is_word(compiler_t *g, const char *w) {
    return g->tok.kind == T_IDENT && !strcmp(g->tok.text, w);
}
static bool accept_punct(compiler_t *g, const char *p) {
    if (!is_punct(g, p)) return false;
    advance(g);
    return true;
}
static bool accept_word(compiler_t *g, const char *w) {
    if (!is_word(g, w)) return false;
    advance(g);
    return true;
}
static void expect_punct(compiler_t *g, const char *p) {
    if (!accept_punct(g, p)) fail(g, "expected something else here");
}

/* ------------------------------------------------------------- registers */

static value_t make_value(uint16_t reg, int comps) {
    value_t v;
    memset(&v, 0, sizeof v);
    v.reg = reg;
    v.swizzle = SH_SWIZZLE_XYZW;
    v.comps = comps;
    return v;
}

static value_t new_temp(compiler_t *g, int comps) {
    if (g->temp_top >= SR_TEMP + SR_TEMP_N) {
        fail(g, "the shader needs more working registers than there are");
        return make_value(SR_TEMP, comps);
    }
    if (g->temp_top > g->high_water) g->high_water = g->temp_top;
    return make_value((uint16_t)g->temp_top++, comps);
}

/* A literal.  Deduplicated, because a shader that mentions 0.0 twenty times
 * should not use twenty registers to say so. */
static value_t constant4(compiler_t *g, float x, float y, float z, float w) {
    sh_shader_t *sh = g->sh;
    for (int i = 0; i < sh->nconstants; i++) {
        if (sh->constants[i][0] == x && sh->constants[i][1] == y &&
            sh->constants[i][2] == z && sh->constants[i][3] == w)
            return make_value((uint16_t)(SR_CONST + i), 4);
    }
    if (sh->nconstants >= SR_CONST_N) {
        fail(g, "the shader has more literals than there is room for");
        return make_value(SR_CONST, 4);
    }
    int i = sh->nconstants++;
    sh->constants[i][0] = x; sh->constants[i][1] = y;
    sh->constants[i][2] = z; sh->constants[i][3] = w;
    return make_value((uint16_t)(SR_CONST + i), 4);
}

static value_t constant1(compiler_t *g, float x) {
    value_t v = constant4(g, x, x, x, x);
    v.comps = 1;
    return v;
}

/* ------------------------------------------------------------- emitting */

static uint8_t mask_for(int comps) {
    return (uint8_t)((1u << comps) - 1u);
}

static int emit3(compiler_t *g, int op, value_t dst, uint8_t mask,
                 value_t a, value_t b, value_t c) {
    sh_shader_t *sh = g->sh;
    if (sh->count >= SH_MAX_INSTRUCTIONS) {
        fail(g, "the shader is longer than this compiler can hold");
        return -1;
    }
    sh_instruction_t *in = &sh->code[sh->count];
    memset(in, 0, sizeof *in);
    in->op = (uint8_t)op;
    in->mask = mask;
    in->dst = dst.reg;
    in->src[0] = a.reg; in->swizzle[0] = a.swizzle;
    in->src[1] = b.reg; in->swizzle[1] = b.swizzle;
    in->src[2] = c.reg; in->swizzle[2] = c.swizzle;
    return sh->count++;
}

static value_t zero_value(void) {
    value_t v;
    memset(&v, 0, sizeof v);
    v.swizzle = SH_SWIZZLE_XYZW;
    return v;
}

static int emit2(compiler_t *g, int op, value_t dst, uint8_t mask,
                 value_t a, value_t b) {
    return emit3(g, op, dst, mask, a, b, zero_value());
}

static int emit1(compiler_t *g, int op, value_t dst, uint8_t mask, value_t a) {
    return emit3(g, op, dst, mask, a, zero_value(), zero_value());
}

/* Broadcasting a scalar: reading it as .xxxx is all it takes. */
static value_t broadcast(value_t v) {
    int c = (v.swizzle >> 0) & 3;
    v.swizzle = (uint8_t)(c | (c << 2) | (c << 4) | (c << 6));
    return v;
}

static value_t swizzle_pick(value_t v, const int *pick, int n) {
    uint8_t out = 0;
    for (int i = 0; i < 4; i++) {
        int idx = pick[i < n ? i : n - 1];
        int comp = (v.swizzle >> (2 * idx)) & 3;
        out |= (uint8_t)(comp << (2 * i));
    }
    v.swizzle = out;
    v.comps = n;
    v.lvalue = false;
    return v;
}

/* ------------------------------------------------------------------ scopes */

static symbol_t *find_local(compiler_t *g, const char *name) {
    for (int i = g->nlocals - 1; i >= 0; i--)
        if (!strcmp(g->local[i].name, name)) return &g->local[i];
    return NULL;
}

static void push_scope(compiler_t *g) {
    if (g->depth < 16) g->scope[g->depth] = g->nlocals;
    g->depth++;
}

static void pop_scope(compiler_t *g) {
    g->depth--;
    if (g->depth >= 0 && g->depth < 16) g->nlocals = g->scope[g->depth];
}

/* --------------------------------------------------------------- types */

typedef struct { const char *name; int comps; bool matrix; bool sampler; } type_t;

static const type_t types[] = {
    { "float",     1, false, false },
    { "int",       1, false, false },
    { "bool",      1, false, false },
    { "vec2",      2, false, false },
    { "vec3",      3, false, false },
    { "vec4",      4, false, false },
    { "ivec2",     2, false, false },
    { "ivec3",     3, false, false },
    { "ivec4",     4, false, false },
    { "mat4",     16, true,  false },
    { "mat3",     16, true,  false },
    { "sampler2D", 1, false, true  },
};

static const type_t *lookup_type(const char *name) {
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (!strcmp(types[i].name, name)) return &types[i];
    return NULL;
}

/* ----------------------------------------------------------- expressions */

static value_t parse_expression(compiler_t *g);
static value_t parse_assignment(compiler_t *g);

/* Making two operands agree.  Only two cases exist and both are ordinary:
 * they already match, or one of them is a scalar that broadcasts. */
static int reconcile(compiler_t *g, value_t *a, value_t *b) {
    if (a->comps == b->comps) return a->comps;
    if (a->comps == 1) { *a = broadcast(*a); a->comps = b->comps; return b->comps; }
    if (b->comps == 1) { *b = broadcast(*b); b->comps = a->comps; return a->comps; }
    fail(g, "these two do not have the same number of components");
    return a->comps;
}

static value_t builtin_call(compiler_t *g, const char *name);

static value_t parse_primary(compiler_t *g) {
    if (g->tok.kind == T_NUMBER) {
        float n = g->tok.number;
        advance(g);
        return constant1(g, n);
    }

    if (accept_punct(g, "(")) {
        value_t v = parse_expression(g);
        expect_punct(g, ")");
        return v;
    }

    if (accept_punct(g, "-")) {
        value_t a = parse_primary(g);
        value_t t = new_temp(g, a.comps);
        emit1(g, SH_NEG, t, mask_for(a.comps), a);
        return t;
    }

    if (accept_punct(g, "+")) return parse_primary(g);

    if (accept_punct(g, "!")) {
        value_t a = parse_primary(g);
        value_t t = new_temp(g, a.comps);
        emit1(g, SH_NOT, t, mask_for(a.comps), a);
        return t;
    }

    if (g->tok.kind == T_IDENT) {
        char name[SH_NAME_MAX];
        strncpy(name, g->tok.text, sizeof name - 1);
        name[sizeof name - 1] = 0;

        /* A constructor or a function is an identifier followed by a bracket,
         * and the two are told apart by whether the name is a type. */
        if (peek(g)->kind == T_PUNCT && !strcmp(peek(g)->text, "(")) {
            advance(g);
            advance(g);
            return builtin_call(g, name);
        }

        advance(g);
        symbol_t *sym = find_local(g, name);
        if (!sym) {
            fail(g, "no such variable");
            return constant1(g, 0);
        }

        value_t v = make_value(sym->reg, sym->is_matrix ? 4 : sym->comps);
        v.is_matrix = sym->is_matrix;
        v.is_sampler = sym->is_sampler;
        v.sampler_unit = sym->sampler_unit;
        v.lvalue = true;
        return v;
    }

    fail(g, "expected a value");
    return constant1(g, 0);
}

static value_t parse_postfix(compiler_t *g) {
    value_t v = parse_primary(g);

    for (;;) {
        if (accept_punct(g, ".")) {
            if (g->tok.kind != T_IDENT) { fail(g, "expected components"); return v; }

            int pick[4] = { 0, 0, 0, 0 };
            int n = 0;
            const char *s = g->tok.text;

            for (; s[n] && n < 4; n++) {
                int c;
                switch (s[n]) {
                case 'x': case 'r': case 's': c = 0; break;
                case 'y': case 'g': case 't': c = 1; break;
                case 'z': case 'b': case 'p': c = 2; break;
                case 'w': case 'a': case 'q': c = 3; break;
                default: fail(g, "that is not a component name"); return v;
                }
                if (c >= v.comps) { fail(g, "that component is not there"); return v; }
                pick[n] = c;
            }
            if (s[n]) { fail(g, "too many components"); return v; }

            bool was_lvalue = v.lvalue;
            v = swizzle_pick(v, pick, n);
            v.lvalue = was_lvalue;
            advance(g);
            continue;
        }

        if (accept_punct(g, "[")) {
            /* Only constant indices, which is all a matrix column or a vector
             * component needs and all a register machine can do without
             * indirect addressing. */
            if (g->tok.kind != T_NUMBER) {
                fail(g, "only a constant index is supported here");
                return v;
            }
            int index = (int)g->tok.number;
            advance(g);
            expect_punct(g, "]");

            if (v.is_matrix) {
                if (index < 0 || index > 3) { fail(g, "no such column"); return v; }
                value_t col = make_value((uint16_t)(v.reg + index), 4);
                v = col;
            } else {
                if (index < 0 || index >= v.comps) {
                    fail(g, "no such component");
                    return v;
                }
                int pick[1] = { index };
                bool was_lvalue = v.lvalue;
                v = swizzle_pick(v, pick, 1);
                v.lvalue = was_lvalue;
            }
            continue;
        }

        break;
    }

    return v;
}

static value_t parse_multiplicative(compiler_t *g) {
    value_t a = parse_postfix(g);

    for (;;) {
        bool mul = is_punct(g, "*");
        bool div = is_punct(g, "/");
        if (!mul && !div) break;
        advance(g);
        value_t b = parse_postfix(g);

        /* A matrix times a vector is the whole point of a vertex shader, and
         * it is not componentwise multiplication. */
        if (a.is_matrix && !b.is_matrix) {
            value_t t = new_temp(g, 4);
            emit2(g, SH_MATMUL, t, 0xF, a, b);
            a = t;
            continue;
        }
        if (a.is_matrix && b.is_matrix) {
            /* Four columns, each transformed by the left matrix. */
            if (g->temp_top + 4 > SR_TEMP + SR_TEMP_N) {
                fail(g, "no room for a matrix product");
                return a;
            }
            int base = g->temp_top;
            g->temp_top += 4;
            if (g->temp_top > g->high_water) g->high_water = g->temp_top;
            for (int i = 0; i < 4; i++) {
                value_t dst = make_value((uint16_t)(base + i), 4);
                value_t col = make_value((uint16_t)(b.reg + i), 4);
                emit2(g, SH_MATMUL, dst, 0xF, a, col);
            }
            value_t t = make_value((uint16_t)base, 4);
            t.is_matrix = true;
            a = t;
            continue;
        }
        if (!a.is_matrix && b.is_matrix) {
            fail(g, "a vector on the left of a matrix is not supported");
            return a;
        }

        int comps = reconcile(g, &a, &b);
        value_t t = new_temp(g, comps);
        emit2(g, mul ? SH_MUL : SH_DIV, t, mask_for(comps), a, b);
        a = t;
    }

    return a;
}

static value_t parse_additive(compiler_t *g) {
    value_t a = parse_multiplicative(g);

    for (;;) {
        bool add = is_punct(g, "+");
        bool sub = is_punct(g, "-");
        if (!add && !sub) break;
        advance(g);
        value_t b = parse_multiplicative(g);
        int comps = reconcile(g, &a, &b);
        value_t t = new_temp(g, comps);
        emit2(g, add ? SH_ADD : SH_SUB, t, mask_for(comps), a, b);
        a = t;
    }

    return a;
}

static value_t parse_relational(compiler_t *g) {
    value_t a = parse_additive(g);

    for (;;) {
        int op;
        if (is_punct(g, "<"))       op = SH_SLT;
        else if (is_punct(g, ">"))  op = SH_SGT;
        else if (is_punct(g, "<=")) op = SH_SLE;
        else if (is_punct(g, ">=")) op = SH_SGE;
        else break;
        advance(g);
        value_t b = parse_additive(g);
        int comps = reconcile(g, &a, &b);
        value_t t = new_temp(g, comps);
        emit2(g, op, t, mask_for(comps), a, b);
        a = t;
    }

    return a;
}

static value_t parse_equality(compiler_t *g) {
    value_t a = parse_relational(g);

    for (;;) {
        int op;
        if (is_punct(g, "=="))      op = SH_SEQ;
        else if (is_punct(g, "!=")) op = SH_SNE;
        else break;
        advance(g);
        value_t b = parse_relational(g);
        int comps = reconcile(g, &a, &b);
        value_t t = new_temp(g, comps);
        emit2(g, op, t, mask_for(comps), a, b);
        a = t;
    }

    return a;
}

static value_t parse_logical(compiler_t *g) {
    value_t a = parse_equality(g);

    for (;;) {
        int op;
        if (is_punct(g, "&&"))      op = SH_AND;
        else if (is_punct(g, "||")) op = SH_OR;
        else break;
        advance(g);
        value_t b = parse_equality(g);
        int comps = reconcile(g, &a, &b);
        value_t t = new_temp(g, comps);
        emit2(g, op, t, mask_for(comps), a, b);
        a = t;
    }

    return a;
}

static value_t parse_conditional(compiler_t *g) {
    value_t cond = parse_logical(g);
    if (!accept_punct(g, "?")) return cond;

    value_t yes = parse_assignment(g);
    expect_punct(g, ":");
    value_t no = parse_assignment(g);

    /* Both sides are evaluated and then selected between, which is what
     * hardware does: a branch per pixel costs more than the arithmetic it
     * avoids. */
    int comps = reconcile(g, &yes, &no);
    value_t c = broadcast(cond);
    value_t diff = new_temp(g, comps);
    emit2(g, SH_SUB, diff, mask_for(comps), yes, no);
    value_t t = new_temp(g, comps);
    emit3(g, SH_MAD, t, mask_for(comps), diff, c, no);
    return t;
}

/* Assigning through a swizzle.  The destination mask says which components
 * change, and the source has to be permuted so that each one takes the right
 * component of the value - which is not the same thing at all. */
static void assign_to(compiler_t *g, value_t dst, value_t src) {
    if (!dst.lvalue) { fail(g, "that cannot be assigned to"); return; }

    if (src.comps == 1 && dst.comps > 1) {
        src = broadcast(src);
        src.comps = dst.comps;
    }
    if (src.comps != dst.comps) {
        fail(g, "the two sides have different numbers of components");
        return;
    }

    uint8_t mask = 0;
    uint8_t permuted = 0;

    for (int i = 0; i < dst.comps; i++) {
        int target = (dst.swizzle >> (2 * i)) & 3;
        if (mask & (1u << target)) {
            fail(g, "the same component is written twice");
            return;
        }
        mask |= (uint8_t)(1u << target);
        int from = (src.swizzle >> (2 * i)) & 3;
        permuted |= (uint8_t)(from << (2 * target));
    }

    /* Components the mask does not cover are never read, so whatever they
     * hold in the permutation does not matter. */
    value_t source = src;
    source.swizzle = permuted;

    value_t target = dst;
    target.swizzle = SH_SWIZZLE_XYZW;
    emit1(g, SH_MOV, target, mask, source);
}

static value_t parse_assignment(compiler_t *g) {
    value_t a = parse_conditional(g);

    int compound = 0;
    if (is_punct(g, "="))       compound = -1;
    else if (is_punct(g, "+=")) compound = SH_ADD;
    else if (is_punct(g, "-=")) compound = SH_SUB;
    else if (is_punct(g, "*=")) compound = SH_MUL;
    else if (is_punct(g, "/=")) compound = SH_DIV;
    else return a;

    advance(g);
    value_t b = parse_assignment(g);

    if (compound != -1) {
        value_t lhs = a;
        int comps = reconcile(g, &lhs, &b);
        value_t t = new_temp(g, comps);
        emit2(g, compound, t, mask_for(comps), lhs, b);
        b = t;
    }

    assign_to(g, a, b);
    return a;
}

static value_t parse_expression(compiler_t *g) { return parse_assignment(g); }

/* ------------------------------------------------------- builtin functions */

static value_t call_normalize(compiler_t *g, value_t a) {
    int op = a.comps == 2 ? SH_DP2 : (a.comps == 3 ? SH_DP3 : SH_DP4);
    value_t len = new_temp(g, 1);
    emit2(g, op, len, 0xF, a, a);
    value_t inv = new_temp(g, 1);
    emit1(g, SH_RSQ, inv, 0xF, len);
    value_t t = new_temp(g, a.comps);
    emit2(g, SH_MUL, t, mask_for(a.comps), a, broadcast(inv));
    return t;
}

static value_t call_length(compiler_t *g, value_t a) {
    int op = a.comps == 2 ? SH_DP2 : (a.comps == 3 ? SH_DP3 : SH_DP4);
    value_t sq = new_temp(g, 1);
    emit2(g, op, sq, 0xF, a, a);
    value_t t = new_temp(g, 1);
    emit1(g, SH_SQRT, t, 0xF, sq);
    return t;
}

static value_t builtin_call(compiler_t *g, const char *name) {
    value_t arg[4];
    int n = 0;

    if (!is_punct(g, ")")) {
        for (;;) {
            if (n >= 4) { fail(g, "too many arguments"); break; }
            arg[n++] = parse_assignment(g);
            if (!accept_punct(g, ",")) break;
        }
    }
    expect_punct(g, ")");
    if (g->failed) return constant1(g, 0);

    /* A constructor: vec3(1.0), vec4(rgb, 1.0), mat4(...) and so on.  The
     * arguments are laid out component by component, which is exactly what the
     * language says they are. */
    const type_t *type = lookup_type(name);
    if (type) {
        if (type->sampler) { fail(g, "a sampler cannot be constructed"); return arg[0]; }

        if (type->comps == 1) {
            if (n != 1) { fail(g, "expected one argument"); return constant1(g, 0); }
            value_t t = new_temp(g, 1);
            emit1(g, SH_MOV, t, 0x1, arg[0]);
            return t;
        }

        int want = type->matrix ? 16 : type->comps;
        if (type->matrix) { fail(g, "building a matrix in a shader is not "
                                    "supported; pass it as a uniform"); }

        value_t t = new_temp(g, want);

        /* One argument that is itself a scalar fills every component. */
        if (n == 1 && arg[0].comps == 1) {
            emit1(g, SH_MOV, t, mask_for(want), broadcast(arg[0]));
            return t;
        }

        int placed = 0;
        for (int i = 0; i < n && placed < want; i++) {
            for (int k = 0; k < arg[i].comps && placed < want; k++) {
                int pick[1] = { k };
                value_t component = swizzle_pick(arg[i], pick, 1);
                value_t dst = t;
                dst.swizzle = SH_SWIZZLE_XYZW;
                emit1(g, SH_MOV, dst, (uint8_t)(1u << placed),
                      broadcast(component));
                placed++;
            }
        }

        if (placed != want) {
            fail(g, "the constructor was given the wrong number of components");
        }
        return t;
    }

    struct { const char *name; int args; } expect[] = {
        { "dot", 2 }, { "cross", 2 }, { "normalize", 1 }, { "length", 1 },
        { "distance", 2 }, { "mix", 3 }, { "clamp", 3 }, { "min", 2 },
        { "max", 2 }, { "abs", 1 }, { "floor", 1 }, { "fract", 1 },
        { "sign", 1 }, { "sqrt", 1 }, { "inversesqrt", 1 }, { "pow", 2 },
        { "exp2", 1 }, { "log2", 1 }, { "exp", 1 }, { "log", 1 },
        { "sin", 1 }, { "cos", 1 }, { "tan", 1 }, { "reflect", 2 },
        { "step", 2 }, { "smoothstep", 3 }, { "texture2D", 2 },
        { "texture", 2 }, { "ceil", 1 }, { "mod", 2 },
    };
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        if (strcmp(name, expect[i].name)) continue;
        if (n != expect[i].args) {
            fail(g, "that function takes a different number of arguments");
            return constant1(g, 0);
        }
        break;
    }

    if (!strcmp(name, "dot")) {
        int comps = reconcile(g, &arg[0], &arg[1]);
        int op = comps == 2 ? SH_DP2 : (comps == 3 ? SH_DP3 : SH_DP4);
        value_t t = new_temp(g, 1);
        emit2(g, op, t, 0xF, arg[0], arg[1]);
        return t;
    }
    if (!strcmp(name, "cross")) {
        value_t t = new_temp(g, 3);
        emit2(g, SH_CROSS, t, 0x7, arg[0], arg[1]);
        return t;
    }
    if (!strcmp(name, "normalize")) return call_normalize(g, arg[0]);
    if (!strcmp(name, "length"))    return call_length(g, arg[0]);
    if (!strcmp(name, "distance")) {
        int comps = reconcile(g, &arg[0], &arg[1]);
        value_t d = new_temp(g, comps);
        emit2(g, SH_SUB, d, mask_for(comps), arg[0], arg[1]);
        return call_length(g, d);
    }
    if (!strcmp(name, "mix")) {
        int comps = reconcile(g, &arg[0], &arg[1]);
        value_t t = arg[2].comps == 1 ? broadcast(arg[2]) : arg[2];
        value_t diff = new_temp(g, comps);
        emit2(g, SH_SUB, diff, mask_for(comps), arg[1], arg[0]);
        value_t out = new_temp(g, comps);
        emit3(g, SH_MAD, out, mask_for(comps), diff, t, arg[0]);
        return out;
    }
    if (!strcmp(name, "clamp")) {
        value_t lo = arg[1].comps == 1 ? broadcast(arg[1]) : arg[1];
        value_t hi = arg[2].comps == 1 ? broadcast(arg[2]) : arg[2];
        int comps = arg[0].comps;
        value_t t = new_temp(g, comps);
        emit2(g, SH_MAX, t, mask_for(comps), arg[0], lo);
        value_t out = new_temp(g, comps);
        emit2(g, SH_MIN, out, mask_for(comps), t, hi);
        return out;
    }
    if (!strcmp(name, "min") || !strcmp(name, "max")) {
        int comps = reconcile(g, &arg[0], &arg[1]);
        value_t t = new_temp(g, comps);
        emit2(g, name[1] == 'i' ? SH_MIN : SH_MAX, t, mask_for(comps),
              arg[0], arg[1]);
        return t;
    }
    if (!strcmp(name, "mod")) {
        /* x - y * floor(x/y), which is what the language defines it as. */
        int comps = reconcile(g, &arg[0], &arg[1]);
        value_t q = new_temp(g, comps);
        emit2(g, SH_DIV, q, mask_for(comps), arg[0], arg[1]);
        value_t f = new_temp(g, comps);
        emit1(g, SH_FLOOR, f, mask_for(comps), q);
        value_t m = new_temp(g, comps);
        emit2(g, SH_MUL, m, mask_for(comps), f, arg[1]);
        value_t out = new_temp(g, comps);
        emit2(g, SH_SUB, out, mask_for(comps), arg[0], m);
        return out;
    }

    struct { const char *name; int op; } simple[] = {
        { "abs", SH_ABS }, { "floor", SH_FLOOR }, { "fract", SH_FRACT },
        { "sign", SH_SIGN }, { "sqrt", SH_SQRT }, { "inversesqrt", SH_RSQ },
        { "exp2", SH_EXP2 }, { "log2", SH_LOG2 },
        { "sin", SH_SIN }, { "cos", SH_COS },
    };
    for (size_t i = 0; i < sizeof simple / sizeof simple[0]; i++) {
        if (strcmp(name, simple[i].name)) continue;
        value_t t = new_temp(g, arg[0].comps);
        emit1(g, simple[i].op, t, mask_for(arg[0].comps), arg[0]);
        return t;
    }

    if (!strcmp(name, "ceil")) {
        /* -floor(-x). */
        value_t neg = new_temp(g, arg[0].comps);
        emit1(g, SH_NEG, neg, mask_for(arg[0].comps), arg[0]);
        value_t f = new_temp(g, arg[0].comps);
        emit1(g, SH_FLOOR, f, mask_for(arg[0].comps), neg);
        value_t t = new_temp(g, arg[0].comps);
        emit1(g, SH_NEG, t, mask_for(arg[0].comps), f);
        return t;
    }

    if (!strcmp(name, "tan")) {
        value_t s = new_temp(g, arg[0].comps);
        emit1(g, SH_SIN, s, mask_for(arg[0].comps), arg[0]);
        value_t c = new_temp(g, arg[0].comps);
        emit1(g, SH_COS, c, mask_for(arg[0].comps), arg[0]);
        value_t t = new_temp(g, arg[0].comps);
        emit2(g, SH_DIV, t, mask_for(arg[0].comps), s, c);
        return t;
    }

    if (!strcmp(name, "exp") || !strcmp(name, "log")) {
        /* Base e through base two, which is the instruction that exists. */
        bool is_exp = name[1] == 'x';
        value_t k = constant1(g, is_exp ? 1.4426950408889634f
                                        : 0.6931471805599453f);
        int comps = arg[0].comps;
        if (is_exp) {
            value_t scaled = new_temp(g, comps);
            emit2(g, SH_MUL, scaled, mask_for(comps), arg[0], broadcast(k));
            value_t t = new_temp(g, comps);
            emit1(g, SH_EXP2, t, mask_for(comps), scaled);
            return t;
        }
        value_t l = new_temp(g, comps);
        emit1(g, SH_LOG2, l, mask_for(comps), arg[0]);
        value_t t = new_temp(g, comps);
        emit2(g, SH_MUL, t, mask_for(comps), l, broadcast(k));
        return t;
    }

    if (!strcmp(name, "pow")) {
        int comps = reconcile(g, &arg[0], &arg[1]);
        value_t t = new_temp(g, comps);
        emit2(g, SH_POW, t, mask_for(comps), arg[0], arg[1]);
        return t;
    }

    if (!strcmp(name, "reflect")) {
        /* I - 2 * dot(N, I) * N. */
        value_t I = arg[0], N = arg[1];
        int comps = reconcile(g, &I, &N);
        int op = comps == 2 ? SH_DP2 : (comps == 3 ? SH_DP3 : SH_DP4);
        value_t d = new_temp(g, 1);
        emit2(g, op, d, 0xF, N, I);
        value_t two = constant1(g, 2.0f);
        value_t scaled = new_temp(g, 1);
        emit2(g, SH_MUL, scaled, 0xF, d, two);
        value_t back = new_temp(g, comps);
        emit2(g, SH_MUL, back, mask_for(comps), N, broadcast(scaled));
        value_t t = new_temp(g, comps);
        emit2(g, SH_SUB, t, mask_for(comps), I, back);
        return t;
    }

    if (!strcmp(name, "step")) {
        int comps = reconcile(g, &arg[0], &arg[1]);
        value_t t = new_temp(g, comps);
        emit2(g, SH_SGE, t, mask_for(comps), arg[1], arg[0]);
        return t;
    }

    if (!strcmp(name, "smoothstep")) {
        value_t e0 = arg[0], e1 = arg[1], x = arg[2];
        int comps = x.comps;
        if (e0.comps == 1 && comps > 1) e0 = broadcast(e0);
        if (e1.comps == 1 && comps > 1) e1 = broadcast(e1);

        value_t num = new_temp(g, comps);
        emit2(g, SH_SUB, num, mask_for(comps), x, e0);
        value_t den = new_temp(g, comps);
        emit2(g, SH_SUB, den, mask_for(comps), e1, e0);
        value_t t = new_temp(g, comps);
        emit2(g, SH_DIV, t, mask_for(comps), num, den);

        value_t zero = constant1(g, 0.0f), one = constant1(g, 1.0f);
        value_t lo = new_temp(g, comps);
        emit2(g, SH_MAX, lo, mask_for(comps), t, broadcast(zero));
        value_t cl = new_temp(g, comps);
        emit2(g, SH_MIN, cl, mask_for(comps), lo, broadcast(one));

        /* t * t * (3 - 2t). */
        value_t three = constant1(g, 3.0f), minus2 = constant1(g, -2.0f);
        value_t inner = new_temp(g, comps);
        emit3(g, SH_MAD, inner, mask_for(comps), cl, broadcast(minus2),
              broadcast(three));
        value_t sq = new_temp(g, comps);
        emit2(g, SH_MUL, sq, mask_for(comps), cl, cl);
        value_t out = new_temp(g, comps);
        emit2(g, SH_MUL, out, mask_for(comps), sq, inner);
        return out;
    }

    if (!strcmp(name, "texture2D") || !strcmp(name, "texture")) {
        if (!arg[0].is_sampler) {
            fail(g, "the first argument has to be a sampler");
            return constant1(g, 0);
        }
        value_t t = new_temp(g, 4);
        sh_shader_t *sh = g->sh;
        if (sh->count >= SH_MAX_INSTRUCTIONS) { fail(g, "shader too long"); return t; }
        sh_instruction_t *in = &sh->code[sh->count++];
        memset(in, 0, sizeof *in);
        in->op = SH_TEX;
        in->mask = 0xF;
        in->dst = t.reg;
        in->src[0] = arg[1].reg;
        in->swizzle[0] = arg[1].swizzle;
        /* Which texture unit comes out of the sampler's own register at run
         * time rather than being fixed here, because glUniform1i is how an
         * application says which unit a sampler names - and it may say it
         * after the shader was compiled. */
        in->src[1] = arg[0].reg;
        in->swizzle[1] = SH_SWIZZLE_XYZW;
        return t;
    }

    fail(g, "no such function");
    return constant1(g, 0);
}

/* ------------------------------------------------------------- statements */

static void parse_statement(compiler_t *g);

static void parse_block(compiler_t *g) {
    push_scope(g);
    while (!g->failed && !is_punct(g, "}") && g->tok.kind != T_END)
        parse_statement(g);
    expect_punct(g, "}");
    pop_scope(g);
}

static void declare_local(compiler_t *g, const type_t *type, const char *name) {
    if (g->nlocals >= MAX_LOCALS) { fail(g, "too many variables"); return; }
    if (type->matrix) { fail(g, "a local matrix is not supported"); return; }

    symbol_t *sym = &g->local[g->nlocals++];
    memset(sym, 0, sizeof *sym);
    strncpy(sym->name, name, SH_NAME_MAX - 1);
    sym->comps = type->comps;

    if (g->local_top >= SR_TEMP + SR_TEMP_N) {
        fail(g, "no room for another variable");
        return;
    }
    sym->reg = (uint16_t)g->local_top++;
    if (g->local_top > g->temp_top) g->temp_top = g->local_top;
}

static void parse_statement(compiler_t *g) {
    if (g->failed) return;

    /* Each statement starts its scratch allocation over: nothing an
     * expression built has to outlive the statement it was built in. */
    g->temp_top = g->local_top;

    if (accept_punct(g, ";")) return;

    if (accept_punct(g, "{")) { parse_block(g); return; }

    if (is_word(g, "discard")) {
        advance(g);
        expect_punct(g, ";");
        value_t none = zero_value();
        emit1(g, SH_DISCARD, none, 0, none);
        g->sh->uses_discard = true;
        return;
    }

    if (is_word(g, "return")) {
        advance(g);
        if (!is_punct(g, ";")) parse_expression(g);
        expect_punct(g, ";");
        value_t none = zero_value();
        emit1(g, SH_END, none, 0, none);
        return;
    }

    if (is_word(g, "if")) {
        advance(g);
        expect_punct(g, "(");
        value_t cond = parse_expression(g);
        expect_punct(g, ")");

        int jump_over = emit1(g, SH_JMPZ, zero_value(), 0, cond);
        parse_statement(g);

        if (is_word(g, "else")) {
            advance(g);
            int jump_end = emit1(g, SH_JMP, zero_value(), 0, zero_value());
            if (jump_over >= 0) g->sh->code[jump_over].target = (int16_t)g->sh->count;
            parse_statement(g);
            if (jump_end >= 0) g->sh->code[jump_end].target = (int16_t)g->sh->count;
        } else {
            if (jump_over >= 0) g->sh->code[jump_over].target = (int16_t)g->sh->count;
        }
        return;
    }

    if (is_word(g, "for")) {
        advance(g);
        expect_punct(g, "(");
        push_scope(g);

        /* The initialiser, which may declare the counter. */
        if (!is_punct(g, ";")) {
            const type_t *type = g->tok.kind == T_IDENT ? lookup_type(g->tok.text) : NULL;
            if (type) {
                advance(g);
                char name[SH_NAME_MAX];
                strncpy(name, g->tok.text, sizeof name - 1);
                name[sizeof name - 1] = 0;
                advance(g);
                declare_local(g, type, name);
                if (accept_punct(g, "=")) {
                    g->temp_top = g->local_top;
                    value_t init = parse_expression(g);
                    symbol_t *sym = find_local(g, name);
                    if (sym) {
                        value_t dst = make_value(sym->reg, sym->comps);
                        dst.lvalue = true;
                        assign_to(g, dst, init);
                    }
                }
            } else {
                parse_expression(g);
            }
        }
        expect_punct(g, ";");

        int top = g->sh->count;
        int jump_out = -1;
        if (!is_punct(g, ";")) {
            g->temp_top = g->local_top;
            value_t cond = parse_expression(g);
            jump_out = emit1(g, SH_JMPZ, zero_value(), 0, cond);
        }
        expect_punct(g, ";");

        /* The step has to run at the end of the body, but it is written here.
         * Recording where the source is and re-parsing it after the body is
         * simpler than any scheme for moving instructions around, and a for
         * loop's step is one expression. */
        int step_at = g->at;
        token_t step_tok = g->tok;
        bool step_have_ahead = g->have_ahead;
        token_t step_ahead = g->ahead;

        int nesting = 0;
        while (g->tok.kind != T_END) {
            if (is_punct(g, "(")) nesting++;
            if (is_punct(g, ")")) { if (!nesting) break; nesting--; }
            advance(g);
        }
        expect_punct(g, ")");

        parse_statement(g);

        /* Now the step, from where it was left. */
        if (!g->failed) {
            int resume_at = g->at;
            token_t resume_tok = g->tok;
            bool resume_have = g->have_ahead;
            token_t resume_ahead = g->ahead;

            g->at = step_at;
            g->tok = step_tok;
            g->have_ahead = step_have_ahead;
            g->ahead = step_ahead;
            g->temp_top = g->local_top;

            if (!is_punct(g, ")")) {
                if (is_punct(g, "+") || is_punct(g, "-")) {
                    /* ++i and --i, which lex as two tokens. */
                    bool up = is_punct(g, "+");
                    advance(g);
                    if (is_punct(g, "+") || is_punct(g, "-")) advance(g);
                    value_t v = parse_postfix(g);
                    value_t one = constant1(g, 1.0f);
                    value_t t = new_temp(g, v.comps);
                    emit2(g, up ? SH_ADD : SH_SUB, t, mask_for(v.comps), v, one);
                    assign_to(g, v, t);
                } else {
                    value_t v = parse_postfix(g);
                    if (is_punct(g, "++") || is_punct(g, "--")) {
                        bool up = g->tok.text[0] == '+';
                        advance(g);
                        value_t one = constant1(g, 1.0f);
                        value_t t = new_temp(g, v.comps);
                        emit2(g, up ? SH_ADD : SH_SUB, t, mask_for(v.comps), v, one);
                        assign_to(g, v, t);
                    } else if (is_punct(g, "=") || is_punct(g, "+=") ||
                               is_punct(g, "-=") || is_punct(g, "*=") ||
                               is_punct(g, "/=")) {
                        int op = -1;
                        if (is_punct(g, "+=")) op = SH_ADD;
                        else if (is_punct(g, "-=")) op = SH_SUB;
                        else if (is_punct(g, "*=")) op = SH_MUL;
                        else if (is_punct(g, "/=")) op = SH_DIV;
                        advance(g);
                        value_t b = parse_expression(g);
                        if (op != -1) {
                            value_t lhs = v;
                            int comps = reconcile(g, &lhs, &b);
                            value_t t = new_temp(g, comps);
                            emit2(g, op, t, mask_for(comps), lhs, b);
                            b = t;
                        }
                        assign_to(g, v, b);
                    }
                }
            }

            g->at = resume_at;
            g->tok = resume_tok;
            g->have_ahead = resume_have;
            g->ahead = resume_ahead;
        }

        int back = emit1(g, SH_JMP, zero_value(), 0, zero_value());
        if (back >= 0) g->sh->code[back].target = (int16_t)top;
        if (jump_out >= 0) g->sh->code[jump_out].target = (int16_t)g->sh->count;

        pop_scope(g);
        return;
    }

    /* A declaration. */
    if (g->tok.kind == T_IDENT) {
        const type_t *type = lookup_type(g->tok.text);
        if (type) {
            advance(g);
            for (;;) {
                if (g->tok.kind != T_IDENT) { fail(g, "expected a name"); return; }
                char name[SH_NAME_MAX];
                strncpy(name, g->tok.text, sizeof name - 1);
                name[sizeof name - 1] = 0;
                advance(g);

                declare_local(g, type, name);

                if (accept_punct(g, "=")) {
                    g->temp_top = g->local_top;
                    value_t init = parse_expression(g);
                    symbol_t *sym = find_local(g, name);
                    if (sym) {
                        value_t dst = make_value(sym->reg, sym->comps);
                        dst.lvalue = true;
                        assign_to(g, dst, init);
                    }
                }

                if (!accept_punct(g, ",")) break;
            }
            expect_punct(g, ";");
            return;
        }
    }

    parse_expression(g);
    expect_punct(g, ";");
}

/* ------------------------------------------------------- top level */

static void add_builtin(compiler_t *g, const char *name, uint16_t reg,
                        int comps) {
    if (g->nlocals >= MAX_LOCALS) return;
    symbol_t *sym = &g->local[g->nlocals++];
    memset(sym, 0, sizeof *sym);
    strncpy(sym->name, name, SH_NAME_MAX - 1);
    sym->reg = reg;
    sym->comps = comps;
}

static void record_symbol(sh_symbol_t *table, int *count, const char *name,
                          uint16_t reg, int comps, bool matrix, bool sampler) {
    if (*count >= SH_MAX_SYMBOLS) return;
    sh_symbol_t *s = &table[(*count)++];
    memset(s, 0, sizeof *s);
    strncpy(s->name, name, SH_NAME_MAX - 1);
    s->reg = reg;
    s->components = (uint8_t)comps;
    s->is_matrix = matrix;
    s->is_sampler = sampler;
}

static void parse_declaration(compiler_t *g, int kind) {
    /* kind: 0 uniform, 1 attribute, 2 varying */
    if (g->tok.kind != T_IDENT) { fail(g, "expected a type"); return; }
    const type_t *type = lookup_type(g->tok.text);
    if (!type) { fail(g, "unknown type"); return; }
    advance(g);

    if (g->tok.kind != T_IDENT) { fail(g, "expected a name"); return; }
    char name[SH_NAME_MAX];
    strncpy(name, g->tok.text, sizeof name - 1);
    name[sizeof name - 1] = 0;
    advance(g);
    expect_punct(g, ";");
    if (g->failed) return;

    if (g->nlocals >= MAX_LOCALS) { fail(g, "too many declarations"); return; }
    symbol_t *sym = &g->local[g->nlocals++];
    memset(sym, 0, sizeof *sym);
    strncpy(sym->name, name, SH_NAME_MAX - 1);
    sym->comps = type->matrix ? 4 : type->comps;
    sym->is_matrix = type->matrix;
    sym->is_sampler = type->sampler;

    if (kind == 0) {
        int need = type->matrix ? 4 : 1;
        if (g->next_uniform + need > SR_UNIFORM + SR_UNIFORM_N) {
            fail(g, "too many uniforms");
            return;
        }
        sym->reg = (uint16_t)g->next_uniform;
        g->next_uniform += need;

        if (type->sampler) {
            sym->sampler_unit = g->samplers++;
            /* A sampler's register holds which texture unit it names, so that
             * setting it is an ordinary uniform assignment. */
        }
        record_symbol(g->sh->uniform, &g->sh->nuniforms, name, sym->reg,
                      type->matrix ? 16 : type->comps, type->matrix,
                      type->sampler);
        if (type->sampler)
            g->sh->uniform[g->sh->nuniforms - 1].location = sym->sampler_unit;
        return;
    }

    if (kind == 1) {
        if (g->next_attrib >= SR_ATTRIB + SR_ATTRIB_N) {
            fail(g, "too many attributes");
            return;
        }
        sym->reg = (uint16_t)g->next_attrib++;
        record_symbol(g->sh->attribute, &g->sh->nattributes, name, sym->reg,
                      type->comps, false, false);
        return;
    }

    if (g->next_varying >= SR_VARYING + SR_VARYING_N) {
        fail(g, "more values passed between the two shaders than there is room "
                "for");
        return;
    }
    sym->reg = (uint16_t)g->next_varying++;
    record_symbol(g->sh->varying, &g->sh->nvaryings, name, sym->reg,
                  type->comps, false, false);
}

bool sh_compile(sh_shader_t *out, sh_stage_t stage, const char *source) {
    static compiler_t g;

    memset(out, 0, sizeof *out);
    out->stage = stage;

    memset(&g, 0, sizeof g);
    g.src = source;
    g.sh = out;
    g.next_uniform = SR_UNIFORM;
    g.next_attrib = SR_ATTRIB;
    g.next_varying = SR_VARYING;

    if (!source) {
        strncpy(out->log, "no source", sizeof out->log - 1);
        return false;
    }

    /* The names the language provides rather than the shader. */
    if (stage == SH_VERTEX) {
        add_builtin(&g, "gl_Position", SR_POSITION, 4);
        add_builtin(&g, "gl_PointSize", SR_POINTSIZE, 1);
    } else {
        add_builtin(&g, "gl_FragColor", SR_FRAGCOLOR, 4);
        add_builtin(&g, "gl_FragCoord", SR_FRAGCOORD, 4);
    }
    advance(&g);

    while (!g.failed && g.tok.kind != T_END) {
        if (accept_word(&g, "precision")) {
            while (!is_punct(&g, ";") && g.tok.kind != T_END) advance(&g);
            expect_punct(&g, ";");
            continue;
        }
        if (accept_word(&g, "invariant") || accept_word(&g, "highp") ||
            accept_word(&g, "mediump") || accept_word(&g, "lowp"))
            continue;

        if (accept_word(&g, "uniform"))  { parse_declaration(&g, 0); continue; }
        if (accept_word(&g, "attribute")){ parse_declaration(&g, 1); continue; }
        if (accept_word(&g, "varying"))  { parse_declaration(&g, 2); continue; }

        /* The modern spelling: "in" and "out".  In a vertex shader an input is
         * an attribute and an output is a varying; in a fragment shader an
         * input is a varying and the single output is the colour. */
        if (accept_word(&g, "in")) {
            parse_declaration(&g, stage == SH_VERTEX ? 1 : 2);
            continue;
        }
        if (accept_word(&g, "out")) {
            if (stage == SH_VERTEX) { parse_declaration(&g, 2); continue; }

            /* A fragment shader's out variable is another name for the colour
             * that comes out, whatever the shader chose to call it. */
            if (g.tok.kind != T_IDENT) { fail(&g, "expected a type"); break; }
            advance(&g);
            if (g.tok.kind != T_IDENT) { fail(&g, "expected a name"); break; }
            add_builtin(&g, g.tok.text, SR_FRAGCOLOR, 4);
            advance(&g);
            expect_punct(&g, ";");
            continue;
        }

        if (accept_word(&g, "const")) {
            /* Treated as an ordinary variable at file scope: it costs a
             * register and behaves identically. */
            if (g.tok.kind != T_IDENT) { fail(&g, "expected a type"); break; }
            const type_t *type = lookup_type(g.tok.text);
            if (!type) { fail(&g, "unknown type"); break; }
            advance(&g);
            if (g.tok.kind != T_IDENT) { fail(&g, "expected a name"); break; }
            char name[SH_NAME_MAX];
            strncpy(name, g.tok.text, sizeof name - 1);
            name[sizeof name - 1] = 0;
            advance(&g);
            g.local_top = g.local_top ? g.local_top : SR_TEMP;
            declare_local(&g, type, name);
            if (accept_punct(&g, "=")) {
                g.temp_top = g.local_top;
                value_t init = parse_expression(&g);
                symbol_t *sym = find_local(&g, name);
                if (sym) {
                    value_t dst = make_value(sym->reg, sym->comps);
                    dst.lvalue = true;
                    assign_to(&g, dst, init);
                }
            }
            expect_punct(&g, ";");
            continue;
        }

        if (accept_word(&g, "void")) {
            if (!accept_word(&g, "main")) { fail(&g, "only main is supported"); break; }
            expect_punct(&g, "(");
            accept_word(&g, "void");
            expect_punct(&g, ")");
            expect_punct(&g, "{");

            /* Locals start after the registers the declarations above took;
             * every one of those was a uniform, attribute or varying, and
             * those live in their own ranges, so the temporary file is
             * untouched and starts at zero.
             *
             * What must not happen here is discarding the symbols those
             * declarations made.  They are the uniforms and attributes the
             * shader is about to use, and a compiler that clears them reports
             * that every one of them is an unknown variable. */
            g.local_top = SR_TEMP;
            g.temp_top = SR_TEMP;
            g.depth = 0;

            parse_block(&g);

            value_t none = zero_value();
            emit1(&g, SH_END, none, 0, none);
            continue;
        }

        fail(&g, "unexpected declaration");
    }

    if (g.failed) return false;

    if (!out->count) {
        strncpy(out->log, "the shader has no main", sizeof out->log - 1);
        return false;
    }

    /* Whether it wrote the one thing its stage exists to produce.  A vertex
     * shader that never sets gl_Position draws nothing, and saying so at
     * compile time is far better than an empty screen. */
    for (int i = 0; i < out->count; i++) {
        if (out->code[i].mask == 0) continue;
        if (out->code[i].dst == SR_POSITION) out->writes_position = true;
        if (out->code[i].dst == SR_FRAGCOLOR) out->writes_colour = true;
    }

    if (stage == SH_VERTEX && !out->writes_position) {
        strncpy(out->log, "the vertex shader never sets gl_Position",
                sizeof out->log - 1);
        return false;
    }
    if (stage == SH_FRAGMENT && !out->writes_colour && !out->uses_discard) {
        strncpy(out->log, "the fragment shader never sets a colour",
                sizeof out->log - 1);
        return false;
    }

    out->compiled = true;
    return true;
}

/* ------------------------------------------------------------------ linking
 *
 * The two stages were compiled apart and each assigned its own varying
 * registers in the order it declared them.  If the fragment shader declares
 * them in a different order - which is allowed, and common - then the vertex
 * shader's second output lands in the register the fragment shader reads as
 * its first.  Matching them up by name is the whole job, and skipping it
 * produces a picture that is subtly, consistently wrong.
 */
bool sh_link(sh_program_t *out, sh_shader_t *vertex, sh_shader_t *fragment) {
    memset(out, 0, sizeof *out);

    if (!vertex || !vertex->compiled || vertex->stage != SH_VERTEX) {
        strncpy(out->log, "no compiled vertex shader", sizeof out->log - 1);
        return false;
    }
    if (!fragment || !fragment->compiled || fragment->stage != SH_FRAGMENT) {
        strncpy(out->log, "no compiled fragment shader", sizeof out->log - 1);
        return false;
    }

    /* Every varying the fragment shader reads must be one the vertex shader
     * writes, and it has to be moved to the vertex shader's register.
     *
     * The whole mapping is worked out before anything is moved.  Rewriting one
     * register at a time is a permutation applied in place: when two varyings
     * swap places, the second rewrite catches the registers the first one just
     * moved and undoes it, and both end up in the same register.  Nothing
     * about that is visible until a shader declares its varyings in a
     * different order from the one that writes them - which is legal, common,
     * and exactly what this file exists to cope with. */
    uint16_t from[SH_MAX_SYMBOLS], to[SH_MAX_SYMBOLS];
    int moves = 0;

    for (int f = 0; f < fragment->nvaryings; f++) {
        sh_symbol_t *want = &fragment->varying[f];
        int found = -1;
        for (int v = 0; v < vertex->nvaryings; v++)
            if (!strcmp(vertex->varying[v].name, want->name)) { found = v; break; }

        if (found < 0) {
            snprintf(out->log, sizeof out->log,
                     "the fragment shader reads \"%s\" and nothing writes it",
                     want->name);
            return false;
        }

        if (moves < SH_MAX_SYMBOLS) {
            from[moves] = want->reg;
            to[moves] = vertex->varying[found].reg;
            moves++;
        }
        want->reg = vertex->varying[found].reg;

        int slot = want->reg - SR_VARYING;
        if (slot >= 0 && slot < SR_VARYING_N) {
            if (want->components > out->varying_components[slot])
                out->varying_components[slot] = want->components;
            if (slot + 1 > out->nvaryings) out->nvaryings = slot + 1;
        }
    }

    /* One pass, reading the original register and writing the new one, so no
     * rewrite can see another rewrite's output. */
    for (int i = 0; i < fragment->count; i++) {
        sh_instruction_t *in = &fragment->code[i];

        /* A sampling instruction's second operand names a sampler, not a
         * value, so it is left alone - but its first operand is the
         * coordinate, and that is very often one of these. */
        int operands = in->op == SH_TEX ? 1 : 3;

        for (int s = 0; s < operands; s++) {
            for (int m = 0; m < moves; m++) {
                if (in->src[s] == from[m]) { in->src[s] = to[m]; break; }
            }
        }
        for (int m = 0; m < moves; m++) {
            if (in->dst == from[m]) { in->dst = to[m]; break; }
        }
    }

    /* The uniforms of both stages, as one table.  A uniform of the same name
     * in both is one uniform with two homes. */
    for (int i = 0; i < SH_MAX_SYMBOLS * 2; i++) {
        out->vertex_reg[i] = -1;
        out->fragment_reg[i] = -1;
    }

    for (int v = 0; v < vertex->nuniforms; v++) {
        int at = out->nuniforms++;
        out->uniform[at] = vertex->uniform[v];
        out->uniform[at].location = at;
        out->vertex_reg[at] = vertex->uniform[v].reg;
    }
    for (int f = 0; f < fragment->nuniforms; f++) {
        int at = -1;
        for (int i = 0; i < out->nuniforms; i++)
            if (!strcmp(out->uniform[i].name, fragment->uniform[f].name)) { at = i; break; }
        if (at < 0) {
            at = out->nuniforms++;
            out->uniform[at] = fragment->uniform[f];
            out->uniform[at].location = at;
        }
        out->fragment_reg[at] = fragment->uniform[f].reg;
    }

    for (int a = 0; a < vertex->nattributes; a++) {
        out->attribute[out->nattributes] = vertex->attribute[a];
        out->attribute[out->nattributes].location = out->nattributes;
        out->nattributes++;
    }

    out->vertex = vertex;
    out->fragment = fragment;
    out->linked = true;
    return true;
}
