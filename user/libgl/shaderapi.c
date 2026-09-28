/* shaderapi.c - the programmable pipeline as OpenGL presents it.
 *
 * glCreateShader, glShaderSource, glCompileShader, glLinkProgram, glUseProgram,
 * the glUniform family and the generic vertex attributes: the interface every
 * engine written since about 2004 is built against, on top of the compiler in
 * glsl.c and the interpreter in shadervm.c.
 *
 * Two things here are worth saying out loud because they are where an
 * implementation of this interface usually goes wrong.
 *
 * Uniforms belong to each linked program, persist across program switches, and
 * reset on a successful relink. Linked executable copies are independent of
 * subsequent source edits, compilation, attachment changes and shader deletion.
 *
 * Which texture unit a sampler names comes out of the sampler's own register
 * at the moment of sampling.  It is an ordinary integer uniform, set with
 * glUniform1i, and a shader compiled before it was set has to pick up the new
 * value without being recompiled.
 */
#include "glstate.h"
#include "../libc/kestrel.h"
#include "../libc/math.h"

/* One machine for each stage, kept between calls so that the uniforms uploaded
 * at the start of a draw are still there for every vertex and every pixel of
 * it. */
static sh_machine_t vs;
static sh_machine_t fs;

/* ------------------------------------------------------------------ objects */

static gl_shader_object_t *shader_of(GLuint id) {
    if (id == 0 || id > GL_MAX_SHADER_OBJECTS) return NULL;
    gl_shader_object_t *o = &g_gl.shader_object[id - 1];
    return o->used ? o : NULL;
}

static gl_program_object_t *program_of(GLuint id) {
    if (id == 0 || id > GL_MAX_PROGRAM_OBJECTS) return NULL;
    gl_program_object_t *o = &g_gl.program_object[id - 1];
    return o->used ? o : NULL;
}

static void shader_unref(GLuint id) {
    gl_shader_object_t *o = shader_of(id);
    if (!o) return;
    if (o->attachments) o->attachments--;
    if (!o->attachments && o->delete_pending) o->used = false;
}

static void program_destroy(gl_program_object_t *p) {
    shader_unref(p->vertex);
    shader_unref(p->fragment);
    free(p->executable);
    memset(p, 0, sizeof *p);
}

GLuint glCreateShader(GLenum type) {
    gl_context_init();

    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        gl_set_error(GL_INVALID_ENUM);
        return 0;
    }

    for (int i = 0; i < GL_MAX_SHADER_OBJECTS; i++) {
        if (g_gl.shader_object[i].used) continue;
        gl_shader_object_t *o = &g_gl.shader_object[i];
        memset(o, 0, sizeof *o);
        o->used = true;
        o->stage = type == GL_VERTEX_SHADER ? SH_VERTEX : SH_FRAGMENT;
        return (GLuint)(i + 1);
    }

    gl_set_error(GL_OUT_OF_MEMORY);
    return 0;
}

void glShaderSource(GLuint shader, GLsizei count, const char *const *string,
                    const GLint *length) {
    gl_context_init();
    gl_shader_object_t *o = shader_of(shader);
    if (!o) { gl_set_error(GL_INVALID_VALUE); return; }

    o->source_len = 0;
    o->source[0] = 0;

    for (GLsizei i = 0; i < count; i++) {
        if (!string || !string[i]) continue;
        int n = length && length[i] >= 0 ? (int)length[i] : (int)strlen(string[i]);
        for (int k = 0; k < n && o->source_len < (int)sizeof o->source - 1; k++)
            o->source[o->source_len++] = string[i][k];
    }
    o->source[o->source_len] = 0;
    o->compiled = false;
}

void glCompileShader(GLuint shader) {
    gl_context_init();
    gl_shader_object_t *o = shader_of(shader);
    if (!o) { gl_set_error(GL_INVALID_VALUE); return; }

    o->compiled = sh_compile(&o->shader, o->stage, o->source);
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    gl_context_init();
    gl_shader_object_t *o = shader_of(shader);
    if (!o || !params) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (pname) {
    case GL_COMPILE_STATUS:   *params = o->compiled ? GL_TRUE : GL_FALSE; break;
    case GL_INFO_LOG_LENGTH:  *params = (GLint)strlen(o->shader.log) + 1; break;
    case GL_SHADER_TYPE:
        *params = o->stage == SH_VERTEX ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
        break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

void glGetShaderInfoLog(GLuint shader, GLsizei cap, GLsizei *written, char *log) {
    gl_context_init();
    gl_shader_object_t *o = shader_of(shader);
    if (!o || !log || cap <= 0) return;
    size_t n = strlcpy(log, o->shader.log, (size_t)cap);
    if (written) *written = (GLsizei)(n < (size_t)cap ? n : (size_t)cap - 1);
}

void glDeleteShader(GLuint shader) {
    gl_context_init();
    gl_shader_object_t *o = shader_of(shader);
    if (o) {
        o->delete_pending = true;
        if (!o->attachments) o->used = false;
    }
}

GLuint glCreateProgram(void) {
    gl_context_init();
    for (int i = 0; i < GL_MAX_PROGRAM_OBJECTS; i++) {
        if (g_gl.program_object[i].used) continue;
        gl_program_object_t *o = &g_gl.program_object[i];
        memset(o, 0, sizeof *o);
        o->used = true;
        return (GLuint)(i + 1);
    }
    gl_set_error(GL_OUT_OF_MEMORY);
    return 0;
}

void glAttachShader(GLuint program, GLuint shader) {
    gl_context_init();
    gl_program_object_t *p = program_of(program);
    gl_shader_object_t *o = shader_of(shader);
    if (!p || !o) { gl_set_error(GL_INVALID_VALUE); return; }

    /* This compiler supports one source object per stage, not multi-unit GLSL. */
    GLuint *slot = o->stage == SH_VERTEX ? &p->vertex : &p->fragment;
    if (*slot) { gl_set_error(GL_INVALID_OPERATION); return; }
    *slot = shader;
    o->attachments++;
}

void glDetachShader(GLuint program, GLuint shader) {
    gl_context_init();
    gl_program_object_t *p = program_of(program);
    if (!p || !shader_of(shader)) { gl_set_error(GL_INVALID_VALUE); return; }
    if (p->vertex == shader) p->vertex = 0;
    else if (p->fragment == shader) p->fragment = 0;
    else { gl_set_error(GL_INVALID_OPERATION); return; }
    shader_unref(shader); /* attachment changes do not alter the executable */
}

void glLinkProgram(GLuint program) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    gl_program_object_t *p = program_of(program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }

    gl_shader_object_t *v = shader_of(p->vertex);
    gl_shader_object_t *f = shader_of(p->fragment);

    if (!v || !f || !v->compiled || !f->compiled) {
        strlcpy(p->program.log, "both shaders have to compile before linking",
                sizeof p->program.log);
        p->linked = false;
        return;
    }

    sh_shader_t *executable = malloc(2 * sizeof *executable);
    if (!executable) {
        strlcpy(p->program.log, "no memory for linked executable", sizeof p->program.log);
        p->linked = false;
        gl_set_error(GL_OUT_OF_MEMORY);
        return;
    }
    executable[0] = v->shader;
    executable[1] = f->shader;
    sh_program_t linked;
    p->linked = sh_link(&linked, &executable[0], &executable[1]);
    if (!p->linked) {
        strlcpy(p->program.log, linked.log, sizeof p->program.log);
        free(executable);
        return; /* a currently installed executable survives a failed relink */
    }
    free(p->executable);
    p->executable = executable;
    p->program = linked;
    memset(p->uniform_value, 0, sizeof p->uniform_value);
    memset(p->uniform_set, 0, sizeof p->uniform_set);
}

void glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    gl_context_init();
    gl_program_object_t *p = program_of(program);
    if (!p || !params) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (pname) {
    case GL_LINK_STATUS:      *params = p->linked ? GL_TRUE : GL_FALSE; break;
    case GL_INFO_LOG_LENGTH:  *params = (GLint)strlen(p->program.log) + 1; break;
    case GL_ACTIVE_UNIFORMS:  *params = p->program.nuniforms; break;
    case GL_ACTIVE_ATTRIBUTES: *params = p->program.nattributes; break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

void glGetProgramInfoLog(GLuint program, GLsizei cap, GLsizei *written, char *log) {
    gl_context_init();
    gl_program_object_t *p = program_of(program);
    if (!p || !log || cap <= 0) return;
    size_t n = strlcpy(log, p->program.log, (size_t)cap);
    if (written) *written = (GLsizei)(n < (size_t)cap ? n : (size_t)cap - 1);
}

void glUseProgram(GLuint program) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (program) {
        gl_program_object_t *p = program_of(program);
        if (!p || !p->linked) { gl_set_error(GL_INVALID_OPERATION); return; }
    }
    gl_program_object_t *old = program_of(g_gl.bound_program);
    g_gl.bound_program = program;
    if (old && old != program_of(program) && old->delete_pending) program_destroy(old);
}

void glDeleteProgram(GLuint program) {
    gl_context_init();
    gl_program_object_t *p = program_of(program);
    if (!p) return;
    if (g_gl.bound_program == program) p->delete_pending = true;
    else program_destroy(p);
}

/* ----------------------------------------------------------------- uniforms */

const sh_program_t *gl_current_program(void) {
    gl_program_object_t *p = program_of(g_gl.bound_program);
    return (p && p->program.linked) ? &p->program : NULL;
}

bool gl_program_active(void) { return gl_current_program() != NULL; }

GLint glGetUniformLocation(GLuint program, const char *name) {
    gl_context_init();
    gl_program_object_t *p = program_of(program);
    if (!p || !p->linked) { gl_set_error(GL_INVALID_OPERATION); return -1; }
    return sh_uniform_location(&p->program, name);
}

GLint glGetAttribLocation(GLuint program, const char *name) {
    gl_context_init();
    gl_program_object_t *p = program_of(program);
    if (!p || !p->linked) { gl_set_error(GL_INVALID_OPERATION); return -1; }
    return sh_attribute_location(&p->program, name);
}

static void set_uniform(GLint location, const float *value, int count) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (location == -1) return;
    gl_program_object_t *p = program_of(g_gl.bound_program);
    if (!p || !p->program.linked || location < 0 || location >= p->program.nuniforms) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    for (int i = 0; i < count && i < 16; i++)
        p->uniform_value[location][i] = value[i];
    p->uniform_set[location] = true;
}

void glUniform1f(GLint l, GLfloat a) { float v[1] = { a }; set_uniform(l, v, 1); }
void glUniform2f(GLint l, GLfloat a, GLfloat b) {
    float v[2] = { a, b }; set_uniform(l, v, 2);
}
void glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) {
    float v[3] = { a, b, c }; set_uniform(l, v, 3);
}
void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
    float v[4] = { a, b, c, d }; set_uniform(l, v, 4);
}
void glUniform1i(GLint l, GLint a) { float v[1] = { (float)a }; set_uniform(l, v, 1); }

void glUniform1fv(GLint l, GLsizei n, const GLfloat *v) { if (n > 0) set_uniform(l, v, 1); }
void glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { if (n > 0) set_uniform(l, v, 2); }
void glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { if (n > 0) set_uniform(l, v, 3); }
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { if (n > 0) set_uniform(l, v, 4); }

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value) {
    gl_context_init();
    if (count < 1 || !value) return;

    float m[16];
    if (transpose) {
        /* Stored column major either way, so a row-major matrix has to be
         * turned round rather than copied. */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                m[c * 4 + r] = value[r * 4 + c];
    } else {
        for (int i = 0; i < 16; i++) m[i] = value[i];
    }
    set_uniform(location, m, 16);
}

/* --------------------------------------------------------------- attributes */

void glEnableVertexAttribArray(GLuint index) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (index >= SR_ATTRIB_N) { gl_set_error(GL_INVALID_VALUE); return; }
    g_gl.attrib_array[index].enabled = true;
}

void glDisableVertexAttribArray(GLuint index) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (index >= SR_ATTRIB_N) { gl_set_error(GL_INVALID_VALUE); return; }
    g_gl.attrib_array[index].enabled = false;
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                           GLboolean normalized, GLsizei stride,
                           const GLvoid *pointer) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    if (index >= SR_ATTRIB_N || size < 1 || size > 4 || stride < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if(type!=GL_FLOAT && type!=GL_DOUBLE && type!=GL_BYTE && type!=GL_UNSIGNED_BYTE &&
       type!=GL_SHORT && type!=GL_UNSIGNED_SHORT && type!=GL_INT && type!=GL_UNSIGNED_INT){
        gl_set_error(GL_INVALID_ENUM);return;
    }

    gl_array_t *a = &g_gl.attrib_array[index];
    a->pointer = pointer;
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->normalized = normalized != 0;
    /* A stride of zero means tightly packed, and what "packed" means depends
     * on the type - so it is worked out here rather than at every fetch. */
    if (a->stride == 0) {
        int width = 4;
        switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: width = 1; break;
        case GL_SHORT: case GL_UNSIGNED_SHORT: width = 2; break;
        case GL_DOUBLE: width = 8; break;
        default: width = 4; break;
        }
        a->stride = width * size;
    }
}

void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    gl_context_init();
    if (index >= SR_ATTRIB_N) {gl_set_error(GL_INVALID_VALUE);return;}
    if(index==0 && g_gl.in_begin){glVertex4f(x,y,z,w);return;}
    g_gl.attrib_constant[index][0] = x;
    g_gl.attrib_constant[index][1] = y;
    g_gl.attrib_constant[index][2] = z;
    g_gl.attrib_constant[index][3] = w;
}

void glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) {
    glVertexAttrib4f(i, x, y, z, 1.0f);
}

/* ------------------------------------------------------------- texture units */

void glActiveTexture(GLenum unit) {
    gl_context_init();
    if(g_gl.in_begin){gl_set_error(GL_INVALID_OPERATION);return;}
    int which = (int)(unit - GL_TEXTURE0);
    if (which < 0 || which >= GL_MAX_TEXTURE_UNITS) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    g_gl.active_unit = which;
    // Image/parameter calls must affect this unit's binding, not whichever
    // texture was last bound on a different unit.
    g_gl.bound_texture = g_gl.unit_texture[which];
}

static void sample_unit(int unit, float s, float t, float *rgba) {
    if (unit < 0 || unit >= GL_MAX_TEXTURE_UNITS) {
        rgba[0] = rgba[1] = rgba[2] = 0.0f;
        rgba[3] = 1.0f;
        return;
    }
    gl_sample_texture(g_gl.unit_texture[unit], s, t, rgba);
}

/* ------------------------------------------------------------------ running */

static void upload_uniforms(sh_machine_t *m, const sh_program_t *p, bool vertex) {
    const gl_program_object_t *owner = program_of(g_gl.bound_program);
    if (!owner) return;
    for (int i = 0; i < p->nuniforms; i++) {
        int reg = vertex ? p->vertex_reg[i] : p->fragment_reg[i];
        if (reg < 0) continue;
        if (!owner->uniform_set[i]) continue;

        const float *value = owner->uniform_value[i];
        if (p->uniform[i].is_matrix) {
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    m->reg[reg + c][r] = value[c * 4 + r];
        } else {
            for (int c = 0; c < 4; c++)
                m->reg[reg][c] = c < p->uniform[i].components ? value[c] : 0.0f;
        }
    }
}

void gl_shader_begin_draw(void) {
    const sh_program_t *p = gl_current_program();
    if (!p) return;

    memset(&vs, 0, sizeof vs);
    memset(&fs, 0, sizeof fs);
    fs.sample = sample_unit;

    /* The literals the compiler found, which live in registers like anything
     * else and never change. */
    for (int i = 0; i < p->vertex->nconstants; i++)
        for (int c = 0; c < 4; c++)
            vs.reg[SR_CONST + i][c] = p->vertex->constants[i][c];
    for (int i = 0; i < p->fragment->nconstants; i++)
        for (int c = 0; c < 4; c++)
            fs.reg[SR_CONST + i][c] = p->fragment->constants[i][c];

    upload_uniforms(&vs, p, true);
    upload_uniforms(&fs, p, false);
}

static void gather_attributes(const sh_program_t *p,int index,float (*reg)[4]) {
    /* Gather this vertex's attributes.  An attribute with no array behind it
     * takes the constant the application set, which is what lets a mesh leave
     * out a colour and still draw. */
    for (int i = 0; i < p->nattributes; i++) {
        const sh_symbol_t *sym = &p->attribute[i];
        float value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        const gl_array_t *a = &g_gl.attrib_array[i];
        if (a->enabled && a->pointer) {
            const unsigned char *base = (const unsigned char *)a->pointer +
                                        (size_t)index * a->stride;
            for (int c = 0; c < a->size && c < 4; c++) {
                switch (a->type) {
                case GL_FLOAT:  value[c] = ((const float *)base)[c]; break;
                case GL_DOUBLE: value[c] = (float)((const double *)base)[c]; break;
                case GL_BYTE:
                    value[c]=((const signed char*)base)[c];
                    if(a->normalized){value[c]/=127.0f;if(value[c]<-1)value[c]=-1;}
                    break;
                case GL_UNSIGNED_BYTE:
                    value[c]=base[c];if(a->normalized)value[c]/=255.0f;break;
                case GL_SHORT:
                    value[c]=((const short*)base)[c];
                    if(a->normalized){value[c]/=32767.0f;if(value[c]<-1)value[c]=-1;}
                    break;
                case GL_UNSIGNED_SHORT:
                    value[c]=((const unsigned short*)base)[c];if(a->normalized)value[c]/=65535.0f;break;
                case GL_INT:
                    value[c]=(float)((const int*)base)[c];
                    if(a->normalized){value[c]/=2147483647.0f;if(value[c]<-1)value[c]=-1;}
                    break;
                case GL_UNSIGNED_INT:
                    value[c]=(float)((const unsigned*)base)[c];if(a->normalized)value[c]/=4294967295.0f;break;
                default: break;
                }
            }
            if (a->size < 4) value[3] = a->size == 3 ? 1.0f : value[3];
        } else {
            for (int c = 0; c < 4; c++) value[c] = g_gl.attrib_constant[i][c];
        }

        for (int c = 0; c < 4; c++) reg[sym->reg-SR_ATTRIB][c] = value[c];
    }
}

void gl_shader_vertex(int index, gl_vertex_t *out) {
    const sh_program_t *p = gl_current_program();
    memset(out, 0, sizeof *out);
    if (!p) return;
    gather_attributes(p,index,&vs.reg[SR_ATTRIB]);

    /* Somewhere sensible to start, so a shader that writes only xyz still
     * produces a usable w. */
    vs.reg[SR_POSITION][0] = vs.reg[SR_POSITION][1] = vs.reg[SR_POSITION][2] = 0.0f;
    vs.reg[SR_POSITION][3] = 1.0f;

    sh_run(&vs, p->vertex);
    g_gl.stat_shader_instructions += (unsigned)vs.executed;

    out->clip.x = vs.reg[SR_POSITION][0];
    out->clip.y = vs.reg[SR_POSITION][1];
    out->clip.z = vs.reg[SR_POSITION][2];
    out->clip.w = vs.reg[SR_POSITION][3];

    for (int i = 0; i < SR_VARYING_N; i++)
        for (int c = 0; c < 4; c++)
            out->varying[i][c] = vs.reg[SR_VARYING + i][c];

    /* The fixed-function fields are left at something harmless so that code
     * which reads them without checking gets white rather than nothing. */
    out->r = out->g = out->b = out->a = 1.0f;
}

static unsigned shader_budget(const sh_shader_t *shader) {
    for(int i=0;i<shader->count;i++){
        const sh_instruction_t *in=&shader->code[i];
        if((in->op==SH_JMP||in->op==SH_JMPZ||in->op==SH_JMPNZ)&&in->target<=i)
            return KSH_MAX_STEPS;
    }
    return (unsigned)shader->count; /* a forward-only path visits each PC at most once */
}
bool gl_shader_snapshot(ksh_dispatch_t *vertex,kshr_job_t *fragment,float seed[SR_REGISTERS][4]) {
    const sh_program_t *p=gl_current_program();
    if(!p || !vertex || !fragment || !seed || !p->vertex || !p->fragment ||
       p->vertex->count<=0 || p->vertex->count>SH_MAX_INSTRUCTIONS ||
       p->fragment->count<=0 || p->fragment->count>SH_MAX_INSTRUCTIONS ||
       p->nattributes<0 || p->nattributes>SR_ATTRIB_N || p->nvaryings<0 || p->nvaryings>SR_VARYING_N)return false;
    for(int i=0;i<p->nattributes;i++)
        if(p->attribute[i].reg<SR_ATTRIB || p->attribute[i].reg>=SR_ATTRIB+SR_ATTRIB_N)return false;
    gl_shader_begin_draw();
    memset(vertex,0,sizeof(*vertex));memset(fragment,0,sizeof(*fragment));
    memcpy(seed,vs.reg,sizeof vs.reg);memcpy(fragment->seed,fs.reg,sizeof fs.reg);
    vertex->code_count=p->vertex->count;vertex->budget=shader_budget(p->vertex);
    fragment->code_count=p->fragment->count;fragment->budget=shader_budget(p->fragment);
    fragment->varying_count=p->nvaryings;
    memcpy(vertex->code,p->vertex->code,(size_t)vertex->code_count*sizeof(*vertex->code));
    memcpy(fragment->code,p->fragment->code,(size_t)fragment->code_count*sizeof(*fragment->code));
    return true;
}
bool gl_shader_vertex_input(int index,float *registers,unsigned lanes,unsigned lane) {
    const sh_program_t *p=gl_current_program();
    if(!p || !registers || index<0 || !lanes || lane>=lanes || p->nattributes<0 || p->nattributes>SR_ATTRIB_N)return false;
    float attributes[SR_ATTRIB_N][4]={{0}};
    for(int i=0;i<p->nattributes;i++)if(p->attribute[i].reg<SR_ATTRIB || p->attribute[i].reg>=SR_ATTRIB+SR_ATTRIB_N)return false;
    gather_attributes(p,index,attributes);
    for(unsigned r=0;r<SR_ATTRIB_N;r++)for(unsigned k=0;k<4;k++)
        registers[((SR_ATTRIB+r)*4+k)*lanes+lane]=attributes[r][k];
    registers[(SR_POSITION*4+3)*lanes+lane]=1;
    return true;
}

bool gl_shader_immediate_input(float x,float y,float z,float w,
                               float *registers,unsigned lanes,unsigned lane) {
    const sh_program_t *p=gl_current_program();
    if(!p || !registers || !lanes || lane>=lanes || p->nattributes<0 || p->nattributes>SR_ATTRIB_N)return false;
    const float position[4]={x,y,z,w};
    for(int i=0;i<p->nattributes;i++){
        unsigned r=p->attribute[i].reg;
        if(r<SR_ATTRIB || r>=SR_ATTRIB+SR_ATTRIB_N)return false;
        const float *value=i==0?position:g_gl.attrib_constant[i];
        for(unsigned k=0;k<4;k++)registers[(r*4+k)*lanes+lane]=value[k];
    }
    registers[(SR_POSITION*4+3)*lanes+lane]=1;
    return true;
}

bool gl_shader_fragment(const float varying[SR_VARYING_N][4],
                        float x, float y, float z, float w, float *rgba) {
    const sh_program_t *p = gl_current_program();
    if (!p) return false;

    for (int i = 0; i < p->nvaryings; i++)
        for (int c = 0; c < 4; c++)
            fs.reg[SR_VARYING + i][c] = varying[i][c];

    fs.reg[SR_FRAGCOORD][0] = x;
    fs.reg[SR_FRAGCOORD][1] = y;
    fs.reg[SR_FRAGCOORD][2] = z;
    fs.reg[SR_FRAGCOORD][3] = w == 0.0f ? 0.0f : 1.0f / w;

    /* A shader that only writes rgb leaves alpha as it found it, so it starts
     * opaque rather than at whatever the last pixel produced. */
    fs.reg[SR_FRAGCOLOR][0] = 0.0f;
    fs.reg[SR_FRAGCOLOR][1] = 0.0f;
    fs.reg[SR_FRAGCOLOR][2] = 0.0f;
    fs.reg[SR_FRAGCOLOR][3] = 1.0f;

    sh_run(&fs, p->fragment);
    g_gl.stat_shader_instructions += (unsigned)fs.executed;

    if (fs.discarded) return false;

    for (int c = 0; c < 4; c++) rgba[c] = fs.reg[SR_FRAGCOLOR][c];
    return true;
}
