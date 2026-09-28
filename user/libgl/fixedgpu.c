/* Fixed-function arithmetic on the native shader VM. Triangle lists keep
 * vertex outputs, clipping/setup and textured fragments resident on GPU.
 * Other topologies retain the transform/readback-to-GPU-raster path. */
#include "glstate.h"
#include "gpuvm.h"

#define FIXED_BATCH 256u
#define FIXED_TRIANGLE_BATCH (KSHS_MAX_TRIANGLES*3u)
static struct {
    gl_gpu_vm_t vm;
    sh_shader_t *shader;
    ksh_dispatch_t *job;
    kshr_job_t *fragment;
    float *input, *output;
    unsigned input_capacity;
    float seed[SR_REGISTERS][4];
    bool input_used[SR_REGISTERS];
    bool compiled;
    bool atlas_valid;
    GLuint atlas_id;
    uint64_t atlas_version,atlas_bytes;
    unsigned atlas_width,atlas_height;
    uint64_t resident_batches;
} fixed;

#define LIGHT_UNIFORMS(I) "uniform vec4 p" #I "; uniform vec4 a" #I "; uniform vec4 d" #I "; uniform vec4 s" #I "; uniform vec4 t" #I ";\n"
#define LIGHT_BODY(I) \
    "if(t" #I ".w>0.0){ L=p" #I ".xyz; att=1.0;" \
    "if(p" #I ".w!=0.0){ L=L-eye.xyz; len=length(L);" \
    "if(len>0.00000001){ den=t" #I ".x+t" #I ".y*len+t" #I ".z*len*len;" \
    "if(den>0.00000001) att=1.0/den; }}" \
    "len=length(L); if(len>0.00000001) L=L/len;" \
    "nd=max(dot(N,L),0.0); acc=acc+att*(ambient.xyz*a" #I ".xyz+diffuse.xyz*d" #I ".xyz*nd);" \
    "if(parameters.x>0.0 && nd>0.0){ H=L+vec3(0.0,0.0,1.0); len=length(H);" \
    "if(len>0.00000001) H=H/len; nh=dot(N,H);" \
    "if(nh>0.0) acc=acc+att*specular.xyz*s" #I ".xyz*pow(nh,parameters.x); }}\n"
static const char source[] =
    "uniform mat4 modelview; uniform mat4 projection; uniform mat4 normal_matrix;\n"
    "uniform vec4 model_ambient; uniform vec4 lighting;\n"
    LIGHT_UNIFORMS(0) LIGHT_UNIFORMS(1) LIGHT_UNIFORMS(2) LIGHT_UNIFORMS(3)
    "attribute vec4 position; attribute vec4 colour; attribute vec4 normal; attribute vec4 ambient;"
    "attribute vec4 diffuse; attribute vec4 specular; attribute vec4 emission; attribute vec4 parameters;\n"
    "varying vec4 result_colour; varying vec4 result_uv;\n"
    "void main(){ vec4 eye=modelview*position; gl_Position=projection*eye; result_colour=colour;"
    "result_uv=vec4(parameters.y,parameters.z,0.0,1.0);"
    "if(lighting.x>0.0){ vec3 N=normal.xyz; float len=length(N);"
    "if(parameters.w>0.0 && len>0.00000001) N=N/len;"
    "N=(normal_matrix*vec4(N,0.0)).xyz; len=length(N); if(len>0.00000001) N=N/len;"
    "vec3 acc=emission.xyz+ambient.xyz*model_ambient.xyz;"
    "vec3 L; vec3 H; float att; float den; float nd; float nh;"
    LIGHT_BODY(0) LIGHT_BODY(1) LIGHT_BODY(2) LIGHT_BODY(3)
    "result_colour=vec4(min(acc,vec3(1.0)),diffuse.w); }}";
#undef LIGHT_BODY
#undef LIGHT_UNIFORMS

static bool fail(const char *stage) {
    char note[220];snprintf(note,sizeof note,
        "native fixed vertex %s failed: transport=%lld lane=%u result=%u; no CPU fallback",
        stage,(long long)fixed.vm.last_error,fixed.vm.bad_lane,fixed.vm.lane_error);
    log_write(3,"gl-fixed",note);
    gl_set_error(fixed.vm.last_error==-ENOMEM?GL_OUT_OF_MEMORY:GL_INVALID_OPERATION);
    g_gl.gpu_fixed_failed=true;gui_gpu_invalidate(g_gl.colour);return false;
}
static bool uniform(const char *name,const float *values,unsigned components) {
    for(int i=0;i<fixed.shader->nuniforms;i++){
        const sh_symbol_t *s=&fixed.shader->uniform[i];
        if(strcmp(s->name,name))continue;
        if(s->components!=components || s->reg<SR_UNIFORM ||
           s->reg+(components+3u)/4u>SR_UNIFORM+SR_UNIFORM_N)return false;
        memcpy(fixed.seed[s->reg],values,components*sizeof(float));return true;
    }
    return false;
}
static bool prepare(unsigned lanes,bool readback) {
    if(!fixed.shader)fixed.shader=malloc(sizeof(*fixed.shader));
    if(!fixed.job)fixed.job=malloc(sizeof(*fixed.job));
    if(lanes>fixed.input_capacity){
        float *input=malloc((size_t)lanes*SR_REGISTERS*16u);
        if(!input){fixed.vm.last_error=-ENOMEM;return fail("staging allocation");}
        free(fixed.input);fixed.input=input;fixed.input_capacity=lanes;
    }
    if(readback && !fixed.output)fixed.output=malloc(FIXED_BATCH*8u*sizeof(float));
    if(!readback){free(fixed.output);fixed.output=NULL;}
    if(!fixed.shader||!fixed.job||!fixed.input||(readback&&!fixed.output)){fixed.vm.last_error=-ENOMEM;return fail("staging allocation");}
    if(!fixed.compiled){
        if(!sh_compile(fixed.shader,SH_VERTEX,source)){
            log_write(3,"gl-fixed",fixed.shader->log);return fail("compile");
        }
        fixed.compiled=true;
    }
    memset(fixed.seed,0,sizeof fixed.seed);
    memcpy(fixed.seed[SR_CONST],fixed.shader->constants,sizeof fixed.shader->constants);
    mat4_t normal=mat4_normal_matrix(&g_gl.modelview[g_gl.mv_top]);
    float lighting[4]={g_gl.lighting?1.f:0.f,0,0,0};
    if(!uniform("modelview",g_gl.modelview[g_gl.mv_top].m,16) ||
       !uniform("projection",g_gl.projection[g_gl.proj_top].m,16) ||
       !uniform("normal_matrix",normal.m,16) ||
       !uniform("model_ambient",g_gl.light_model_ambient,4) ||
       !uniform("lighting",lighting,4))return fail("uniform layout");
    for(unsigned i=0;i<GL_MAX_LIGHTS;i++){
        const gl_light_t *l=&g_gl.lights[i];
        float attenuation[4]={l->attenuation[0],l->attenuation[1],l->attenuation[2],l->enabled?1.f:0.f};
        const float *values[5]={l->position,l->ambient,l->diffuse,l->specular,attenuation};
        const char prefixes[5]={'p','a','d','s','t'};
        for(unsigned j=0;j<5;j++){
            char name[3]={prefixes[j],(char)('0'+i),0};
            if(!uniform(name,values[j],4))return fail("light layout");
        }
    }
    memset(fixed.job,0,sizeof(*fixed.job));
    fixed.job->code_count=fixed.shader->count;fixed.job->budget=fixed.shader->count;
    memcpy(fixed.job->code,fixed.shader->code,fixed.shader->count*sizeof(sh_instruction_t));
    gl_gpu_vertex_registers(fixed.job,fixed.input_used);
    if(!fixed.vm.texture_valid && !gl_gpu_vm_set_textures(&fixed.vm,NULL,0))return fail("empty texture state");
    return true;
}
static bool vertex_input(const gl_vertex_t *v,unsigned lane,unsigned lanes) {
    float colour[4]={v->r,v->g,v->b,v->a};
    /* Eight attributes are already occupied by captured lighting state. The
     * unused parameters.yz carry UV without replacing normal/material data. */
    float parameters[4]={v->varying[5][0],v->s,v->t,v->varying[5][3]};
    const float *values[8]={&v->clip.x,colour,v->varying[0],v->varying[1],
        v->varying[2],v->varying[3],v->varying[4],parameters};
    if(fixed.shader->nattributes!=8)return fail("attribute count");
    for(unsigned a=0;a<8;a++){
        unsigned reg=fixed.shader->attribute[a].reg;
        if(reg<SR_ATTRIB || reg>=SR_ATTRIB+SR_ATTRIB_N)return fail("attribute layout");
        for(unsigned k=0;k<4;k++)fixed.input[(reg*4+k)*lanes+lane]=values[a][k];
    }
    return true;
}
void gl_fixed_capture(float x,float y,float z,float w,gl_vertex_t *v) {
    memset(v,0,sizeof(*v));v->clip=vec4_make(x,y,z,w);v->fixed_pending=true;
    v->r=g_gl.cur_colour[0];v->g=g_gl.cur_colour[1];v->b=g_gl.cur_colour[2];v->a=g_gl.cur_colour[3];
    v->s=g_gl.cur_texcoord[0];v->t=g_gl.cur_texcoord[1];
    memcpy(v->varying[0],g_gl.cur_normal,3*sizeof(float));
    /* Material changes are legal within Begin/End: snapshot them per vertex. */
    memcpy(v->varying[1],g_gl.colour_material?g_gl.cur_colour:g_gl.material.ambient,16);
    memcpy(v->varying[2],g_gl.colour_material?g_gl.cur_colour:g_gl.material.diffuse,16);
    memcpy(v->varying[3],g_gl.material.specular,16);
    memcpy(v->varying[4],g_gl.material.emission,16);
    v->varying[5][0]=g_gl.material.shininess;
    v->varying[5][3]=g_gl.normalize?1.f:0.f;
}
bool gl_fixed_transform_gpu(gl_vertex_t *vertices,unsigned count) {
    if(g_gl.gpu_fixed_failed)return false;
    bool pending=false;for(unsigned i=0;i<count;i++)pending|=vertices[i].fixed_pending;
    if(!pending)return true;
    if(!gl_gpu_flush() || !gui_gpu_flush())return fail("pending graphics flush");
    if(!prepare(FIXED_BATCH,true))return false;
    unsigned cursor=0;
    while(cursor<count){
        unsigned index[FIXED_BATCH],n=0;
        while(cursor<count && n<FIXED_BATCH){if(vertices[cursor].fixed_pending)index[n++]=cursor;cursor++;}
        if(!n)continue;
        for(unsigned r=0;r<SR_REGISTERS;r++)if(fixed.input_used[r])for(unsigned k=0;k<4;k++)for(unsigned lane=0;lane<n;lane++)
            fixed.input[(r*4+k)*n+lane]=fixed.seed[r][k];
        for(unsigned lane=0;lane<n;lane++){
            if(!vertex_input(&vertices[index[lane]],lane,n))return false;
        }
        fixed.job->lanes=n;
        if(!gl_gpu_vm_execute_vertex(&fixed.vm,fixed.job,fixed.input,(uint64_t)n*SR_REGISTERS*4))return fail("dispatch");
        for(unsigned lane=0;lane<n;lane++){
            if(fixed.vm.results[lane].result!=KSH_COMPLETE){
                fixed.vm.bad_lane=lane;fixed.vm.lane_error=fixed.vm.results[lane].result;
                return fail("completion");
            }
            g_gl.stat_shader_instructions+=fixed.vm.results[lane].executed;
        }
        if(fixed.shader->nvaryings!=2 || fixed.shader->varying[0].reg!=SR_VARYING ||
           fixed.shader->varying[1].reg!=SR_VARYING+1)return fail("output layout");
        if(!gl_gpu_vm_read(&fixed.vm,SR_POSITION,1,fixed.output,n*4u) ||
           !gl_gpu_vm_read(&fixed.vm,SR_VARYING,1,fixed.output+n*4u,n*4u))return fail("readback");
        for(unsigned lane=0;lane<n;lane++){
            gl_vertex_t *v=&vertices[index[lane]];float *position=&v->clip.x;
            for(unsigned k=0;k<4;k++)position[k]=fixed.output[k*n+lane];
            v->r=fixed.output[4*n+lane];v->g=fixed.output[5*n+lane];
            v->b=fixed.output[6*n+lane];v->a=fixed.output[7*n+lane];
            v->fixed_pending=false;
        }
    }
    return true;
}

static void fragment_instruction(unsigned op,unsigned dst,unsigned mask,unsigned a,unsigned b,
                                  unsigned swizzle_a,unsigned swizzle_b) {
    sh_instruction_t *in=&fixed.fragment->code[fixed.fragment->code_count++];
    *in=(sh_instruction_t){.op=op,.dst=dst,.mask=mask,.src={a,b,0},
        .swizzle={swizzle_a,swizzle_b,SH_SWIZZLE_XYZW}};
}
static bool triangle_fragment(void) {
    if(!fixed.fragment)fixed.fragment=malloc(sizeof(*fixed.fragment));
    if(!fixed.fragment){fixed.vm.last_error=-ENOMEM;return fail("fragment allocation");}
    kshr_job_t *j=fixed.fragment;memset(j,0,sizeof(*j));
    j->varying_count=2;j->origin_lower_left=!g_gl.clip_depth_zero_to_one;
    /* Invalid/incomplete textures leave fixed primitives untextured. A white
     * empty sampler is NOT equivalent for REPLACE or DECAL. */
    gl_texture_t *texture=NULL;
    if(g_gl.texture_2d && g_gl.bound_texture<GL_MAX_TEXTURES){
        gl_texture_t *t=&g_gl.textures[g_gl.bound_texture];
        if(t->used && t->texels && t->width>0 && t->height>0)texture=t;
    }
    uint64_t bytes=0;
    if(texture){
        if(texture->width>32768 || texture->height>32768){fixed.vm.last_error=-EINVAL;return fail("texture dimensions");}
        bytes=(uint64_t)(unsigned)texture->width*(unsigned)texture->height*4u;
        if(bytes>256ull*1024u*1024u){fixed.vm.last_error=-EINVAL;return fail("texture size");}
    }
    bool reusable=fixed.atlas_valid && fixed.vm.texture_valid && fixed.vm.texture_bytes==bytes &&
        fixed.atlas_bytes==bytes && (!texture ||
        (texture->content_version && fixed.atlas_id==g_gl.bound_texture &&
         fixed.atlas_version==texture->content_version && fixed.atlas_width==(unsigned)texture->width &&
         fixed.atlas_height==(unsigned)texture->height));
    if(!reusable){
        fixed.atlas_valid=false;
        if(!gl_gpu_vm_set_textures(&fixed.vm,texture?texture->texels:NULL,bytes))return fail("texture upload");
        fixed.atlas_id=texture?g_gl.bound_texture:0;
        fixed.atlas_version=texture?texture->content_version:0;fixed.atlas_bytes=bytes;
        fixed.atlas_width=texture?texture->width:0;fixed.atlas_height=texture?texture->height:0;
        fixed.atlas_valid=!texture || texture->content_version!=0;
    }
    unsigned identity=SH_SWIZZLE_XYZW;
    if(texture){
        // Rebuild descriptors for every draw, even when image bytes are cached.
        j->texture_count=1;j->textures[0]=(ksh_texture_t){.width=texture->width,.height=texture->height,
            .pitch=texture->width,.wrap_s=texture->wrap_s==GL_REPEAT?0u:1u,
            .wrap_t=texture->wrap_t==GL_REPEAT?0u:1u,.filter=texture->mag_filter==GL_NEAREST?0u:1u};
        fragment_instruction(SH_TEX,0,15,SR_VARYING+1,SR_UNIFORM,identity,identity);
        if(g_gl.tex_env==GL_REPLACE){
            fragment_instruction(SH_MOV,SR_FRAGCOLOR,15,0,0,identity,identity);
        }else if(g_gl.tex_env==GL_DECAL){
            for(unsigned k=0;k<4;k++)j->seed[SR_CONST][k]=1;
            fragment_instruction(SH_SUB,1,15,SR_CONST,0,identity,255); // 1 - texel.aaaa
            fragment_instruction(SH_MUL,SR_FRAGCOLOR,7,SR_VARYING,1,identity,identity);
            fragment_instruction(SH_MUL,2,7,0,0,identity,255);
            fragment_instruction(SH_ADD,SR_FRAGCOLOR,7,SR_FRAGCOLOR,2,identity,identity);
            fragment_instruction(SH_MOV,SR_FRAGCOLOR,8,SR_VARYING,0,identity,identity);
        }else fragment_instruction(SH_MUL,SR_FRAGCOLOR,15,SR_VARYING,0,identity,identity);
    }else fragment_instruction(SH_MOV,SR_FRAGCOLOR,15,SR_VARYING,0,identity,identity);
    fragment_instruction(SH_END,0,0,0,0,identity,identity);
    j->budget=j->code_count;
    return true;
}

bool gl_fixed_triangles_gpu(const gl_vertex_t *vertices,unsigned count) {
    if(g_gl.gpu_fixed_failed)return false;
    if(!g_gl.colour || !g_gl.colour->gpu || !vertices || !count || count%3u){
        fixed.vm.last_error=-EINVAL;return fail("triangle input");
    }
    // Reject mixed transformed/raw batches before any submission, never replay
    // already transformed vertices through a second modelview/projection.
    for(unsigned i=0;i<count;i++)if(!vertices[i].fixed_pending){
        fixed.vm.last_error=-EINVAL;return fail("triangle capture state");
    }
    if(!gl_gpu_flush() || !gui_gpu_flush())return fail("pending graphics flush");
    rect_t clip=rect_intersection(gl_gpu_viewport(),surface_clip(g_gl.colour));
    if(rect_empty(clip))return true;
    unsigned batch=count<FIXED_TRIANGLE_BATCH?count:FIXED_TRIANGLE_BATCH;
    if(!prepare(batch,false) || !triangle_fragment())return false;
    if(fixed.shader->nvaryings!=2 || fixed.shader->varying[0].reg!=SR_VARYING ||
       fixed.shader->varying[1].reg!=SR_VARYING+1)return fail("resident output layout");
    gui_gpu_target_t target;
    if(!gui_gpu_target(g_gl.colour,g_gl.depth_test,&target))return fail("target/depth allocation");
    kshr_job_t *fragment=fixed.fragment;
    fragment->width=g_gl.colour->width;fragment->height=g_gl.colour->height;
    fragment->pitch=target.pitch/4;fragment->depth_pitch=target.depth_pitch/4;
    kshs_config_t setup={.version=KSHS_ABI,.varying_count=2,
        .framebuffer_width=g_gl.colour->width,.framebuffer_height=g_gl.colour->height,
        .viewport_x=g_gl.vp_x,.viewport_y=g_gl.vp_y,.viewport_w=g_gl.vp_w,.viewport_h=g_gl.vp_h,
        .clip_x=clip.x,.clip_y=clip.y,.clip_w=clip.w,.clip_h=clip.h,
        .clip_zero_to_one=g_gl.clip_depth_zero_to_one,.clip_y_down=g_gl.clip_y_down,
        .cull_enable=g_gl.cull,.front_ccw=g_gl.front_face==GL_CCW,
        .cull_face=g_gl.cull_face==GL_FRONT?KSHS_CULL_FRONT:
                   g_gl.cull_face==GL_FRONT_AND_BACK?KSHS_CULL_BOTH:KSHS_CULL_BACK,
        .depth_near=g_gl.depth_near,.depth_far=g_gl.depth_far,
        .flat_varying_mask=g_gl.shade_model==GL_FLAT?1u:0u};
    gl_gpu_fragment_state(&setup.state,0);
    for(unsigned cursor=0;cursor<count;){
        unsigned n=count-cursor;if(n>batch)n=batch;
        for(unsigned r=0;r<SR_REGISTERS;r++)if(fixed.input_used[r])for(unsigned k=0;k<4;k++)for(unsigned lane=0;lane<n;lane++)
            fixed.input[(r*4+k)*n+lane]=fixed.seed[r][k];
        for(unsigned lane=0;lane<n;lane++)if(!vertex_input(vertices+cursor+lane,lane,n))return false;
        fixed.job->lanes=n;
        if(!gl_gpu_vm_execute_vertex(&fixed.vm,fixed.job,fixed.input,(uint64_t)n*SR_REGISTERS*4))return fail("resident vertex dispatch");
        for(unsigned lane=0;lane<n;lane++){
            if(fixed.vm.results[lane].result!=KSH_COMPLETE){
                fixed.vm.last_error=-EIO;fixed.vm.bad_lane=lane;fixed.vm.lane_error=fixed.vm.results[lane].result;
                return fail("resident vertex completion");
            }
            g_gl.stat_shader_instructions+=fixed.vm.results[lane].executed;
        }
        setup.lanes=n;setup.triangle_count=n/3u;
        if(!gl_gpu_vm_geometry(&fixed.vm,target.colour,target.depth,&setup,fragment))return fail("resident triangle setup/raster");
        g_gl.stat_triangles+=fixed.vm.geometry_triangles;
        g_gl.stat_shader_instructions+=(unsigned)fixed.vm.geometry_instructions;
        cursor+=n;
    }
    if(!fixed.resident_batches++)log_write(1,"gl-fixed",
        "resident fixed triangle-list retired: GPU transform/lighting, clipping/setup, texture and raster; no CPU vertex readback or clipping");
    return true;
}
void gl_fixed_gpu_release(void) {
    /* VM release retains unfenced device allocations; never reset its state. */
    gl_gpu_vm_release(&fixed.vm);
    free(fixed.shader);fixed.shader=NULL;fixed.compiled=false;
    free(fixed.job);fixed.job=NULL;free(fixed.input);fixed.input=NULL;free(fixed.output);fixed.output=NULL;
    free(fixed.fragment);fixed.fragment=NULL;fixed.input_capacity=0;
    fixed.atlas_valid=false;fixed.atlas_id=0;fixed.atlas_version=fixed.atlas_bytes=0;
    fixed.atlas_width=fixed.atlas_height=0;
}
