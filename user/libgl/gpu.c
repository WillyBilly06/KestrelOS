/* Application fixed-function triangles use the native surface API. CPU work is
 * vertex transformation/clipping and command assembly, never pixel/depth fill.
 * Every primitive flush retires before toolkit painting can reuse its canvas.
 */
#include "glstate.h"
#include "../libc/math.h"

static kg3d_command_t commands[KG3D_MAX_COMMANDS];
static unsigned command_count;
static surface_t *command_texture;
static rect_t command_bounds;
static uint64_t texture_clock, native_batches;

bool gl_gpu_flush(void) {
    if(!gl_shader_gpu_flush())return false;
    if(!command_count)return true;
    bool primitives=false;
    for(unsigned i=0;i<command_count;i++)
        primitives|=!(commands[i].flags&(KG3D_CLEAR_COLOUR|KG3D_CLEAR_DEPTH));
    bool ok=gui_gpu_draw3d(g_gl.colour,command_texture,commands,command_count,command_bounds);
    command_count=0;command_texture=NULL;
    if(!ok){gl_set_error(GL_OUT_OF_MEMORY);return false;}
    if(primitives && !native_batches++)
        log_write(1,"gl","GPU application primitives: native surface batch retired; no CPU color/depth raster");
    return true;
}

void gl_gpu_texture_release(gl_texture_t *t) {
    if(!t || !t->gpu_image)return;
    gl_gpu_flush(); /* queued records still reference the old immutable image */
    surface_destroy(t->gpu_image);t->gpu_image=NULL;t->gpu_used=0;
}

static surface_t *texture_image(gl_texture_t *t) {
    if(!t->texels || t->width<=0 || t->height<=0)return NULL;
    if(!t->gpu_image) {
        unsigned resident=0;gl_texture_t *oldest=NULL;
        for(unsigned i=0;i<GL_MAX_TEXTURES;i++) {
            gl_texture_t *other=&g_gl.textures[i];
            if(other->gpu_image) {
                resident++;
                if(!oldest || other->gpu_used<oldest->gpu_used)oldest=other;
            }
        }
        /* Leave surface slots for desktop windows/depth; CPU texture sources
         * remain available for re-upload after LRU eviction. */
        if(resident>=4)gl_gpu_texture_release(oldest);
        if(!gl_gpu_flush())return NULL;
        t->gpu_image=gui_gpu_image(t->texels,t->width,t->height);
        if(!t->gpu_image)return NULL;
    }
    t->gpu_used=++texture_clock;return t->gpu_image;
}

static bool enqueue(kg3d_command_t c,surface_t *texture) {
    rect_t area=rect_intersection(rect_make(c.x,c.y,c.width,c.height),surface_clip(g_gl.colour));
    if(rect_empty(area))return true;
    if(command_count && (command_texture!=texture || command_count==KG3D_MAX_COMMANDS))
        if(!gl_gpu_flush())return false;
    command_bounds=command_count?rect_union(command_bounds,area):area;
    command_texture=texture;commands[command_count++]=c;
    return true;
}

rect_t gl_gpu_viewport(void) {
    /* API integer coordinates may be extreme; intersect in int64 before
     * narrowing so a negative/wrapped extent never reaches the GPU. */
    int64_t x=g_gl.vp_x,y=g_gl.vp_y;
    int64_t right=x+g_gl.vp_w,bottom=y+g_gl.vp_h;
    if(g_gl.scissor_on) {
        if(x<g_gl.sc_x)x=g_gl.sc_x;if(y<g_gl.sc_y)y=g_gl.sc_y;
        int64_t sr=(int64_t)g_gl.sc_x+g_gl.sc_w,sb=(int64_t)g_gl.sc_y+g_gl.sc_h;
        if(right>sr)right=sr;if(bottom>sb)bottom=sb;
    }
    if(x<0)x=0;if(y<0)y=0;
    if(right>g_gl.colour->width)right=g_gl.colour->width;
    if(bottom>g_gl.colour->height)bottom=g_gl.colour->height;
    if(x>=right || y>=bottom)return rect_make(0,0,0,0);
    return rect_make((int)x,(int)y,(int)(right-x),(int)(bottom-y));
}
static unsigned comparison(GLenum f) {
    return f>=GL_NEVER && f<=GL_ALWAYS ? f-GL_NEVER : KG3D_ALWAYS;
}
static unsigned blend_factor(GLenum f) {
    switch(f) {
    case GL_ZERO:return KG3D_ZERO;case GL_SRC_ALPHA:return KG3D_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA:return KG3D_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA:return KG3D_DST_ALPHA;case GL_ONE_MINUS_DST_ALPHA:return KG3D_ONE_MINUS_DST_ALPHA;
    default:return KG3D_ONE;
    }
}

void gl_gpu_fragment_state(kg3d_command_t *c,unsigned kind) {
    c->flags=kind;c->depth_func=comparison(g_gl.depth_func);
    c->alpha_func=comparison(g_gl.alpha_func);
    c->blend_src=blend_factor(g_gl.blend_src);c->blend_dst=blend_factor(g_gl.blend_dst);
    c->depth_bias=kind?0:g_gl.poly_offset_units;c->alpha_ref=g_gl.alpha_ref;
    if(g_gl.depth_test)c->flags|=KG3D_DEPTH_TEST;
    if(g_gl.depth_test && g_gl.depth_write)c->flags|=KG3D_DEPTH_WRITE;
    if(g_gl.alpha_test)c->flags|=KG3D_ALPHA_TEST;
    if(g_gl.blend)c->flags|=KG3D_BLEND;
}

bool gl_gpu_clear(GLbitfield mask) {
    if(!g_gl.colour || !g_gl.colour->gpu)return false;
    rect_t r=gl_gpu_viewport();
    kg3d_command_t c={.x=r.x,.y=r.y,.width=r.w,.height=r.h};
    if(mask&GL_COLOR_BUFFER_BIT)c.flags|=KG3D_CLEAR_COLOUR;
    if(mask&GL_DEPTH_BUFFER_BIT)c.flags|=KG3D_CLEAR_DEPTH;
    c.v[0].rgba[0]=g_gl.clear_r;c.v[0].rgba[1]=g_gl.clear_g;
    c.v[0].rgba[2]=g_gl.clear_b;c.v[0].rgba[3]=g_gl.clear_a;c.v[0].z=g_gl.clear_depth;
    if(!__builtin_isfinite(c.v[0].z) || !__builtin_isfinite(c.v[0].rgba[0]) ||
       !__builtin_isfinite(c.v[0].rgba[1]) || !__builtin_isfinite(c.v[0].rgba[2]) ||
       !__builtin_isfinite(c.v[0].rgba[3])) {gl_set_error(GL_INVALID_VALUE);return true;}
    if(c.flags && !rect_empty(r)) {
        if(!enqueue(c,NULL) || !gl_gpu_flush())gl_set_error(GL_OUT_OF_MEMORY);
    }
    return true; /* a failed GPU operation must never enter the CPU clear loop */
}

static bool primitive(const kg3d_vertex_t *a,const kg3d_vertex_t *b,const kg3d_vertex_t *d,unsigned kind,
                       const float *av,const float *bv,const float *cv) {
    if(!g_gl.colour || !g_gl.colour->gpu)return false;
    rect_t r=gl_gpu_viewport();
    if(rect_empty(r))return true;
    const kg3d_vertex_t *vertices[3]={a,b,d};
    for(unsigned i=0;i<3;i++) {
        const kg3d_vertex_t *v=vertices[i];
        if(!(fabsf(v->x)<=1048576.0f) || !(fabsf(v->y)<=1048576.0f) ||
           !__builtin_isfinite(v->z) || !(v->inv_w>0) || !__builtin_isfinite(v->inv_w)) {
            gl_set_error(GL_INVALID_VALUE);return true;
        }
        for(unsigned k=0;k<4;k++)if(!__builtin_isfinite(v->rgba[k])){gl_set_error(GL_INVALID_VALUE);return true;}
        for(unsigned k=0;k<2;k++)if(!__builtin_isfinite(v->uv[k])){gl_set_error(GL_INVALID_VALUE);return true;}
    }
    if(!__builtin_isfinite(g_gl.poly_offset_units) || !__builtin_isfinite(g_gl.alpha_ref)) {
        gl_set_error(GL_INVALID_VALUE);return true;
    }
    float left=a->x,right=a->x,top=a->y,bottom=a->y;
    for(unsigned i=1;i<3;i++) {
        if(vertices[i]->x<left)left=vertices[i]->x;if(vertices[i]->x>right)right=vertices[i]->x;
        if(vertices[i]->y<top)top=vertices[i]->y;if(vertices[i]->y>bottom)bottom=vertices[i]->y;
    }
    /* Triangles quantize to 1/16 pixels; points/lines truncate toward zero,
     * which includes pixel zero for coordinates between -1 and 0. */
    float fringe=kind?1.0f:0.0625f;
    int x=(int)floorf(left-fringe),y=(int)floorf(top-fringe);
    r=rect_intersection(r,rect_make(x,y,(int)ceilf(right+fringe)-x,(int)ceilf(bottom+fringe)-y));
    if(rect_empty(r))return true;
    kg3d_command_t c={.v={*a,*b,*d},.x=r.x,.y=r.y,.width=r.w,.height=r.h};
    gl_gpu_fragment_state(&c,kind);
    if(av && bv && cv){
        kshr_command_t program={.raster=c};
        memcpy(program.varying[0],av,sizeof program.varying[0]);
        memcpy(program.varying[1],bv,sizeof program.varying[1]);
        memcpy(program.varying[2],cv,sizeof program.varying[2]);
        if(!gl_shader_gpu_enqueue(&program))gl_set_error(GL_OUT_OF_MEMORY);
        return true;
    }
    surface_t *texture=NULL;
    if(!kind && g_gl.texture_2d && g_gl.bound_texture<GL_MAX_TEXTURES) {
        gl_texture_t *t=&g_gl.textures[g_gl.bound_texture];
        if(t->used && t->texels && t->width>0 && t->height>0) {
            texture=texture_image(t);
            if(!texture){gl_set_error(GL_OUT_OF_MEMORY);return true;}
            c.flags|=KG3D_TEXTURE;c.filter=t->mag_filter==GL_NEAREST?KG3D_NEAREST:KG3D_LINEAR;
            c.wrap_s=t->wrap_s==GL_REPEAT?KG3D_REPEAT:KG3D_CLAMP;
            c.wrap_t=t->wrap_t==GL_REPEAT?KG3D_REPEAT:KG3D_CLAMP;
            c.tex_env=g_gl.tex_env==GL_REPLACE?KG3D_REPLACE:g_gl.tex_env==GL_DECAL?KG3D_DECAL:KG3D_MODULATE;
        }
    }
    if(!enqueue(c,texture))gl_set_error(GL_OUT_OF_MEMORY);
    return true;
}

bool gl_gpu_triangle(const kg3d_vertex_t *a,const kg3d_vertex_t *b,const kg3d_vertex_t *c) {
    return primitive(a,b,c,0,NULL,NULL,NULL);
}
bool gl_gpu_line(const kg3d_vertex_t *a,const kg3d_vertex_t *b) {
    return primitive(a,b,b,KG3D_LINE,NULL,NULL,NULL);
}
bool gl_gpu_point(const kg3d_vertex_t *a) {
    return primitive(a,a,a,KG3D_POINT,NULL,NULL,NULL);
}
bool gl_gpu_shader_primitive(const kg3d_vertex_t *a,const kg3d_vertex_t *b,const kg3d_vertex_t *c,
                             const float *av,const float *bv,const float *cv,unsigned kind) {
    return primitive(a,b,c,kind,av,bv,cv);
}

/* Compatibility boundary for paths not ported yet. Preserve GPU colour AND
 * depth before any CPU raster work. Do not execute this on a GPU failure. */
bool gl_gpu_software(const char *reason) {
    surface_t *s=g_gl.colour;
    if(!s || !s->gpu)return true;
    if(!gl_gpu_flush() || !gui_gpu_flush()){gl_set_error(GL_OUT_OF_MEMORY);return false;}
    float *depth=malloc((size_t)s->width*s->height*sizeof(float));
    if(!depth){gl_set_error(GL_OUT_OF_MEMORY);return false;}
    if(!gui_gpu_depth_readback(s,depth,s->width) || !gui_gpu_cpu_access(s)) {
        free(depth);gl_set_error(GL_OUT_OF_MEMORY);return false;
    }
    free(g_gl.depth);g_gl.depth=depth;g_gl.depth_w=s->width;g_gl.depth_h=s->height;
    g_gl.gpu_target=false;
    log_write(2,"gl",reason);return true;
}
