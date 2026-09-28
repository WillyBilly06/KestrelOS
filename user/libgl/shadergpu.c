/* Native programmable application pipeline. Triangle-list draws
 * keep assembly/clipping and vertex outputs on GPU. Other topologies retain
 * CPU primitive clipping; vertex/fragment bytecode and pixel writes stay GPU.
 * A single caller owns g_gl and this session. No per-pixel CPU shader fallback.
 */
#include "glstate.h"
#include "gpuvm.h"

#define VERTEX_BATCH 256u
#define IMMEDIATE_TRIANGLE_BATCH ((VERTEX_BATCH/3u)*3u)
/* Array counts are known up front. Use the existing VM lane/work ceiling to
 * amortize upload/dispatch/readback waits; immediate-mode storage stays small.
 * Only staging grows here: primitive history and draw order remain draw-wide. */
#define ARRAY_VERTEX_BATCH KSH_MAX_LANES
#define TRIANGLE_VERTEX_BATCH (KSHS_MAX_TRIANGLES*3u)
_Static_assert(IMMEDIATE_TRIANGLE_BATCH>=3u && IMMEDIATE_TRIANGLE_BATCH<=TRIANGLE_VERTEX_BATCH,
               "immediate triangle batch must contain whole admitted primitives");
_Static_assert((uint64_t)ARRAY_VERTEX_BATCH*KSH_MAX_STEPS<=KSH_MAX_WORK,
               "array vertex batch must admit the maximum shader budget");
static struct {
    gl_gpu_vm_t vm;
    ksh_dispatch_t *vertex;
    kshr_job_t *fragment;
    kshr_command_t pending[KG3D_MAX_COMMANDS];
    float seed[SR_REGISTERS][4];
    float *inputs,*outputs;
    unsigned capacity,output_capacity;
    gui_gpu_target_t target;
    surface_t *canvas;
    rect_t bounds;
    bool active,failed;
    unsigned immediate_count,immediate_pending,immediate_batch;
    GLenum immediate_mode;
    gl_vertex_t immediate_previous[4],immediate_origin;
    uint64_t batches;
    uint64_t geometry_batches;
    GLuint atlas_ids[KSH_MAX_TEXTURES];
    uint64_t atlas_versions[KSH_MAX_TEXTURES];
    bool atlas_valid;
    bool input_used[SR_REGISTERS];
} pipeline;

static bool failure(const char *stage) {
    pipeline.failed=true;
    gl_set_error(pipeline.vm.last_error==-ENOMEM?GL_OUT_OF_MEMORY:GL_INVALID_OPERATION);
    char note[200];snprintf(note,sizeof note,"native programmable %s failed: transport=%lld lane=%u result=%u; no CPU fallback",
        stage,(long long)pipeline.vm.last_error,pipeline.vm.bad_lane,pipeline.vm.lane_error);
    log_write(3,"gl-shader",note);gui_gpu_invalidate(pipeline.canvas);return false;
}
bool gl_shader_gpu_active(void) {return pipeline.active;}

/* A fragment batch can contain small, widely separated primitives. Charging
 * every primitive against the entire union creates thousands of unnecessarily
 * small, synchronous dispatches. Keep the original ordered records immutable
 * and admit only records intersecting each disjoint pixel region. */
static unsigned region_work(rect_t *region,unsigned count) {
    rect_t bounds=rect_make(0,0,0,0);unsigned work=0;
    for(unsigned i=0;i<count;i++){
        const kg3d_command_t *c=&pipeline.pending[i].raster;
        rect_t hit=rect_intersection(*region,rect_make(c->x,c->y,c->width,c->height));
        if(rect_empty(hit))continue;
        bounds=work?rect_union(bounds,hit):hit;
        work+=(c->flags&KG3D_LINE)?KG3D_LINE_WORK:1u;
    }
    *region=bounds;return work;
}
static uint64_t region_cost(rect_t region,unsigned work) {
    return (uint64_t)(unsigned)region.w*(unsigned)region.h*work;
}
static bool raster_tile(rect_t tile,unsigned count) {
    kshr_job_t *job=pipeline.fragment;job->command_count=0;
    for(unsigned i=0;i<count;i++){
        const kshr_command_t *c=&pipeline.pending[i];
        if(!rect_empty(rect_intersection(tile,rect_make(c->raster.x,c->raster.y,
                c->raster.width,c->raster.height))))
            job->commands[job->command_count++]=*c;
    }
    if(!job->command_count)return true;
    job->clip_x=tile.x;job->clip_y=tile.y;job->clip_w=tile.w;job->clip_h=tile.h;
    if(!gl_gpu_vm_raster(&pipeline.vm,pipeline.target.colour,pipeline.target.depth,job))
        return failure("fragment tile");
    for(unsigned i=0;i<pipeline.vm.lanes;i++)g_gl.stat_shader_instructions+=pipeline.vm.results[i].executed;
    return true;
}
static bool raster_region(rect_t region,unsigned count) {
    unsigned work=region_work(&region,count);
    if(!work)return true;
    uint64_t limit=KSH_MAX_WORK/((uint64_t)work*pipeline.fragment->budget);
    if(!limit)return failure("work admission");
    if(kshr_region_cost(pipeline.pending,count,pipeline.fragment->budget,
        region.x,region.y,(unsigned)region.w,(unsigned)region.h)<=KSH_MAX_WORK)return raster_tile(region,count);

    /* Spatial subdivision must actually reduce the admitted work. Dense,
     * overlapping scenes retain the original packed strip tiling instead of
     * being rounded up to a power-of-two number of dispatches. Splits halve a
     * positive extent, so recursion is bounded by the coordinate bit width. */
    uint64_t cost=region_cost(region,work),best=cost;
    rect_t selected[2];bool split=false;
    for(unsigned axis=0;axis<2;axis++){
        int extent=axis?region.h:region.w;if(extent<2)continue;
        rect_t child[2]={region,region};int half=extent/2;
        if(axis){child[0].h=half;child[1].y+=half;child[1].h-=half;}
        else {child[0].w=half;child[1].x+=half;child[1].w-=half;}
        unsigned a=region_work(&child[0],count),b=region_work(&child[1],count);
        uint64_t candidate=region_cost(child[0],a)+region_cost(child[1],b);
        if(candidate<best){best=candidate;selected[0]=child[0];selected[1]=child[1];split=true;}
    }
    if(split && best<=cost-cost/4)
        return raster_region(selected[0],count)&&raster_region(selected[1],count);

    unsigned tw=(unsigned)region.w;if(tw>limit)tw=(unsigned)limit;
    unsigned th=(unsigned)(limit/tw);
    for(unsigned y=0;y<(unsigned)region.h;y+=th){
        unsigned height=(unsigned)region.h-y;if(height>th)height=th;
        for(unsigned x=0;x<(unsigned)region.w;x+=tw){
            unsigned width=(unsigned)region.w-x;if(width>tw)width=tw;
            if(!raster_tile(rect_make(region.x+x,region.y+y,width,height),count))return false;
        }
    }
    return true;
}
bool gl_shader_gpu_flush(void) {
    if(!pipeline.active)return true;
    if(pipeline.failed)return false;
    kshr_job_t *job=pipeline.fragment;
    if(!job->command_count)return true;
    unsigned count=job->command_count;
    memcpy(pipeline.pending,job->commands,count*sizeof(*pipeline.pending));
    if(!raster_region(pipeline.bounds,count))return false;
    job->command_count=0;
    if(!pipeline.batches++)log_write(1,"gl-shader","native programmable application batch retired: GPU vertex/fragment bytecode and raster, no CPU pixel/depth target");
    return true;
}
bool gl_shader_gpu_enqueue(const kshr_command_t *command) {
    if(!pipeline.active || pipeline.failed || !command)return false;
    rect_t area=rect_intersection(rect_make(command->raster.x,command->raster.y,
        command->raster.width,command->raster.height),surface_clip(pipeline.canvas));
    if(rect_empty(area))return true;
    kshr_job_t *job=pipeline.fragment;
    if(job->command_count==KG3D_MAX_COMMANDS && !gl_shader_gpu_flush())return false;
    pipeline.bounds=job->command_count?rect_union(pipeline.bounds,area):area;
    kshr_command_t *out=&job->commands[job->command_count++];*out=*command;
    out->raster.x=area.x;out->raster.y=area.y;out->raster.width=area.w;out->raster.height=area.h;
    return true;
}
void gl_shader_gpu_release(void) {
    if(pipeline.active)return;
    gl_gpu_vm_release(&pipeline.vm); // never reset a quarantined session
    free(pipeline.vertex);pipeline.vertex=NULL;
    free(pipeline.fragment);pipeline.fragment=NULL;
    free(pipeline.inputs);pipeline.inputs=NULL;free(pipeline.outputs);pipeline.outputs=NULL;
    pipeline.capacity=pipeline.output_capacity=0;pipeline.canvas=NULL;
    pipeline.atlas_valid=false;
}
static bool buffers(unsigned lanes,bool readback) {
    if(!pipeline.vertex)pipeline.vertex=malloc(sizeof(*pipeline.vertex));
    if(!pipeline.fragment)pipeline.fragment=malloc(sizeof(*pipeline.fragment));
    if(!pipeline.vertex || !pipeline.fragment){pipeline.vm.last_error=-ENOMEM;return false;}
    if(lanes>pipeline.capacity){
        float *in=malloc((size_t)lanes*SR_REGISTERS*16);
        if(!in){pipeline.vm.last_error=-ENOMEM;return false;}
        free(pipeline.inputs);pipeline.inputs=in;pipeline.capacity=lanes;
    }
    if(readback && lanes>pipeline.output_capacity){
        float *out=malloc((size_t)lanes*(SR_VARYING_N+1u)*16);
        if(!out){pipeline.vm.last_error=-ENOMEM;return false;}
        free(pipeline.outputs);pipeline.outputs=out;pipeline.output_capacity=lanes;
    }else if(!readback){
        free(pipeline.outputs);pipeline.outputs=NULL;pipeline.output_capacity=0;
    }
    return true;
}
static bool textures(void) {
    uint64_t bytes=0;
    GLuint ids[KSH_MAX_TEXTURES]={0};
    uint64_t versions[KSH_MAX_TEXTURES]={0};
    bool cacheable=true;
    for(unsigned unit=0;unit<KSH_MAX_TEXTURES;unit++){
        GLuint id=g_gl.unit_texture[unit];
        if(id>=GL_MAX_TEXTURES)continue;
        const gl_texture_t *t=&g_gl.textures[id];
        if(!t->used || !t->texels || t->width<=0 || t->height<=0)continue;
        if(t->width>32768 || t->height>32768)return false;
        ids[unit]=id;versions[unit]=t->content_version;
        if(!versions[unit])cacheable=false;
        uint64_t size=(uint64_t)(unsigned)t->width*(unsigned)t->height*4;
        if(size>256ull*1024*1024-bytes)return false;
        ksh_texture_t d={.offset=bytes,.width=t->width,.height=t->height,.pitch=t->width,
            .wrap_s=t->wrap_s==GL_REPEAT?0u:1u,.wrap_t=t->wrap_t==GL_REPEAT?0u:1u,
            .filter=t->mag_filter==GL_NEAREST?0u:1u};
        pipeline.vertex->textures[unit]=d;pipeline.fragment->textures[unit]=d;bytes+=size;
    }
    // Sampler descriptors come from current API state every draw; only immutable
    // image bytes are reused. A new image/name gets a distinct content version.
    pipeline.vertex->texture_count=KSH_MAX_TEXTURES;pipeline.fragment->texture_count=KSH_MAX_TEXTURES;
    if(cacheable && pipeline.atlas_valid && pipeline.vm.texture_valid &&
       pipeline.vm.texture_bytes==bytes &&
       !memcmp(ids,pipeline.atlas_ids,sizeof ids) &&
       !memcmp(versions,pipeline.atlas_versions,sizeof versions))return true;
    pipeline.atlas_valid=false;
    unsigned char *atlas=bytes?malloc((size_t)bytes):NULL;
    if(bytes&&!atlas){pipeline.vm.last_error=-ENOMEM;return false;}
    for(unsigned unit=0;unit<KSH_MAX_TEXTURES;unit++){
        const ksh_texture_t *d=&pipeline.vertex->textures[unit];
        if(d->width)memcpy(atlas+d->offset,g_gl.textures[g_gl.unit_texture[unit]].texels,(size_t)d->width*d->height*4);
    }
    bool ok=gl_gpu_vm_set_textures(&pipeline.vm,atlas,bytes);free(atlas);
    if(ok){
        memcpy(pipeline.atlas_ids,ids,sizeof ids);
        memcpy(pipeline.atlas_versions,versions,sizeof versions);
        pipeline.atlas_valid=cacheable;
    }
    return ok;
}

/* Stream already GPU-transformed vertices across dispatch boundaries. Index
 * parity, fan origin, line-loop closure and incomplete groups are draw-wide,
 * not restarted each time a scratch batch is reused. */
static void assemble(GLenum mode,unsigned index,const gl_vertex_t *vertex,
                      gl_vertex_t previous[4],gl_vertex_t *first) {
    unsigned slot=index&3u;previous[slot]=*vertex;
    if(!index)*first=*vertex;
    switch(mode){
    case GL_POINTS:gl_raster_point(vertex,1);break;
    case GL_LINES:if(index&1)gl_raster_line(&previous[(index-1)&3u],vertex);break;
    case GL_LINE_STRIP:case GL_LINE_LOOP:
        if(index)gl_raster_line(&previous[(index-1)&3u],vertex);break;
    case GL_TRIANGLES:
        if(index%3u==2)gl_raster_triangle(&previous[(index-2)&3u],&previous[(index-1)&3u],vertex);break;
    case GL_TRIANGLE_STRIP:
        if(index>=2){
            unsigned a=(index-2)&3u,b=(index-1)&3u;
            if(index&1){unsigned t=a;a=b;b=t;}
            gl_raster_triangle(&previous[a],&previous[b],vertex);
        }break;
    case GL_TRIANGLE_FAN:case GL_POLYGON:
        if(index>=2)gl_raster_triangle(first,&previous[(index-1)&3u],vertex);break;
    case GL_QUADS:
        if((index&3u)==3){
            gl_raster_triangle(&previous[0],&previous[1],&previous[2]);
            gl_raster_triangle(&previous[0],&previous[2],vertex);
        }break;
    case GL_QUAD_STRIP:
        if(index>=3 && (index&1)){
            const gl_vertex_t *a=&previous[(index-3)&3u],*b=&previous[(index-2)&3u],*c=&previous[(index-1)&3u];
            gl_raster_triangle(a,b,vertex);gl_raster_triangle(a,vertex,c);
        }break;
    }
}
static bool start_draw(unsigned batch,bool resident) {
    pipeline.canvas=g_gl.colour;pipeline.failed=false;
    if(!gl_gpu_flush() || !gui_gpu_flush())return failure("pending graphics flush");
    pipeline.active=true;
    if(!buffers(batch,!resident))return failure("CPU staging allocation");
    if(!gl_shader_snapshot(pipeline.vertex,pipeline.fragment,pipeline.seed))return failure("linked program snapshot");
    gl_gpu_vertex_registers(pipeline.vertex,pipeline.input_used);
    if(!textures())return failure("texture snapshot");
    if(!gui_gpu_target(pipeline.canvas,g_gl.depth_test,&pipeline.target))return failure("target/depth allocation");
    pipeline.fragment->width=pipeline.canvas->width;pipeline.fragment->height=pipeline.canvas->height;
    pipeline.fragment->pitch=pipeline.target.pitch/4;pipeline.fragment->depth_pitch=pipeline.target.depth_pitch/4;
    // Native GL uses lower-left gl_FragCoord; Vulkan/D3D bridge coordinates are top-left.
    pipeline.fragment->origin_lower_left=!g_gl.clip_depth_zero_to_one;
    return true;
}
static void finish_draw(void) {
    if(pipeline.fragment)pipeline.fragment->command_count=0;
    pipeline.active=false;
}
static bool run_geometry(unsigned n) {
    if(!gl_shader_gpu_flush() || !gui_gpu_flush())return failure("resident pending flush");
    rect_t clip=rect_intersection(gl_gpu_viewport(),surface_clip(pipeline.canvas));
    if(rect_empty(clip))return true;
    kshs_config_t setup={.version=KSHS_ABI,.lanes=n,.triangle_count=n/3u,
        .varying_count=pipeline.fragment->varying_count,
        .framebuffer_width=pipeline.canvas->width,.framebuffer_height=pipeline.canvas->height,
        .viewport_x=g_gl.vp_x,.viewport_y=g_gl.vp_y,.viewport_w=g_gl.vp_w,.viewport_h=g_gl.vp_h,
        .clip_x=clip.x,.clip_y=clip.y,.clip_w=clip.w,.clip_h=clip.h,
        .clip_zero_to_one=g_gl.clip_depth_zero_to_one,.clip_y_down=g_gl.clip_y_down,
        .cull_enable=g_gl.cull,.front_ccw=g_gl.front_face==GL_CCW,
        .cull_face=g_gl.cull_face==GL_FRONT?KSHS_CULL_FRONT:
                   g_gl.cull_face==GL_FRONT_AND_BACK?KSHS_CULL_BOTH:KSHS_CULL_BACK,
        .depth_near=g_gl.depth_near,.depth_far=g_gl.depth_far};
    gl_gpu_fragment_state(&setup.state,0);
    if(!gl_gpu_vm_geometry(&pipeline.vm,pipeline.target.colour,pipeline.target.depth,
                           &setup,pipeline.fragment))return failure("resident triangle setup/raster");
    g_gl.stat_triangles+=pipeline.vm.geometry_triangles;
    g_gl.stat_shader_instructions+=(unsigned)pipeline.vm.geometry_instructions;
    if(!pipeline.geometry_batches++)log_write(1,"gl-shader",
        "resident triangle-list batch retired: GPU vertex, clipping/setup, fragment and raster; no CPU vertex readback or clipping");
    return true;
}

static bool run_vertices(GLenum mode,unsigned start,unsigned n,
                          gl_vertex_t previous[4],gl_vertex_t *origin,bool resident) {
    pipeline.vertex->lanes=n;
    if(!gl_gpu_vm_execute_vertex(&pipeline.vm,pipeline.vertex,pipeline.inputs,(uint64_t)n*SR_REGISTERS*4))
        return failure("vertex dispatch");
    for(unsigned lane=0;lane<n;lane++){
        if(pipeline.vm.results[lane].result!=KSH_COMPLETE){
            pipeline.vm.last_error=-EIO;pipeline.vm.bad_lane=lane;
            pipeline.vm.lane_error=pipeline.vm.results[lane].result;
            return failure("vertex discard");
        }
        g_gl.stat_shader_instructions+=pipeline.vm.results[lane].executed;
    }
    if(resident)return run_geometry(n);
    unsigned varyings=pipeline.fragment->varying_count;
    if(varyings>SR_VARYING_N)return failure("varying count");
    if(!gl_gpu_vm_read(&pipeline.vm,SR_POSITION,1,pipeline.outputs,(uint64_t)n*4) ||
       (varyings && !gl_gpu_vm_read(&pipeline.vm,SR_VARYING,varyings,pipeline.outputs+n*4,(uint64_t)n*varyings*4)))
        return failure("vertex readback");
    for(unsigned lane=0;lane<n && !pipeline.failed;lane++){
        gl_vertex_t v={0};float *position=&v.clip.x;
        for(unsigned k=0;k<4;k++)position[k]=pipeline.outputs[k*n+lane];
        for(unsigned r=0;r<varyings;r++)for(unsigned k=0;k<4;k++)
            v.varying[r][k]=pipeline.outputs[(4+r*4+k)*n+lane];
        v.r=v.g=v.b=v.a=1;
        assemble(mode,start+lane,&v,previous,origin);
    }
    return !pipeline.failed;
}
bool gl_shader_draw_gpu(GLenum mode,GLint first,GLsizei count,GLenum type,const void *indices) {
    if(!g_gl.colour || !g_gl.colour->gpu || !gl_program_active())return false;
    if(g_gl.in_begin || pipeline.active){gl_set_error(GL_INVALID_OPERATION);return true;}
    if(mode>GL_POLYGON){gl_set_error(GL_INVALID_ENUM);return true;}
    if(count<0 || first<0){gl_set_error(GL_INVALID_VALUE);return true;}
    if(indices && type!=GL_UNSIGNED_BYTE && type!=GL_UNSIGNED_SHORT && type!=GL_UNSIGNED_INT){gl_set_error(GL_INVALID_ENUM);return true;}
    bool resident=mode==GL_TRIANGLES;
    if(resident)count-=count%3; // incomplete final primitive has no observable outputs
    if(!count)return true;
    unsigned limit=resident?TRIANGLE_VERTEX_BATCH:ARRAY_VERTEX_BATCH;
    unsigned batch=(unsigned)count;if(batch>limit)batch=limit;
    if(!start_draw(batch,resident))goto done;
    {
        gl_vertex_t previous[4],origin;
        for(unsigned start=0;start<(unsigned)count && !pipeline.failed;start+=batch){
            unsigned n=(unsigned)count-start;if(n>batch)n=batch;
            pipeline.vertex->lanes=n;
            for(unsigned r=0;r<SR_REGISTERS;r++)if(pipeline.input_used[r])for(unsigned k=0;k<4;k++)for(unsigned lane=0;lane<n;lane++)
                pipeline.inputs[(r*4+k)*n+lane]=pipeline.seed[r][k];
            for(unsigned lane=0;lane<n;lane++){
                uint64_t index=(uint64_t)(unsigned)first+start+lane;
                if(indices){
                    unsigned at=start+lane;
                    if(type==GL_UNSIGNED_BYTE)index=((const unsigned char*)indices)[at];
                    else if(type==GL_UNSIGNED_SHORT)index=((const unsigned short*)indices)[at];
                    else index=((const unsigned*)indices)[at];
                }
                if(index>0x7fffffffu || !gl_shader_vertex_input((int)index,pipeline.inputs,n,lane)){failure("vertex attributes");break;}
            }
            if(pipeline.failed)break;
            if(!run_vertices(mode,start,n,previous,&origin,resident))break;
        }
        if(!pipeline.failed && mode==GL_LINE_LOOP && count>=2)
            gl_raster_line(&previous[((unsigned)count-1)&3u],&origin);
        if(!pipeline.failed)gl_shader_gpu_flush();
    }
done:
    finish_draw();return true; // never demote a failed native draw
}

/* Immediate-mode generic attributes are captured when a vertex is submitted,
 * never fetched from enabled arrays or reconstructed from the final constants.
 * Triangles use 255 lanes so no primitive crosses a resident dispatch; other
 * topologies retain 256 lanes and their existing draw-wide assembly history. */
void gl_shader_immediate_begin_gpu(GLenum mode) {
    pipeline.immediate_count=pipeline.immediate_pending=0;
    pipeline.immediate_mode=mode;
    bool resident=mode==GL_TRIANGLES;
    pipeline.immediate_batch=resident?IMMEDIATE_TRIANGLE_BATCH:VERTEX_BATCH;
    (void)start_draw(pipeline.immediate_batch,resident);
}
void gl_shader_immediate_vertex_gpu(float x,float y,float z,float w) {
    if(pipeline.failed)return;
    unsigned batch=pipeline.immediate_batch;
    if(!pipeline.active || !batch || pipeline.immediate_count>0xffffffffu-batch){failure("immediate vertex count");return;}
    unsigned lane=pipeline.immediate_pending;
    for(unsigned r=0;r<SR_REGISTERS;r++)if(pipeline.input_used[r])for(unsigned k=0;k<4;k++)
        pipeline.inputs[(r*4+k)*batch+lane]=pipeline.seed[r][k];
    if(!gl_shader_immediate_input(x,y,z,w,pipeline.inputs,batch,lane)){
        failure("immediate attributes");return;
    }
    if(++pipeline.immediate_pending==batch){
        (void)run_vertices(pipeline.immediate_mode,pipeline.immediate_count,batch,
                            pipeline.immediate_previous,&pipeline.immediate_origin,
                            pipeline.immediate_mode==GL_TRIANGLES);
        pipeline.immediate_count+=batch;
        pipeline.immediate_pending=0;
    }
}
void gl_shader_immediate_end_gpu(void) {
    unsigned n=pipeline.immediate_pending;
    bool resident=pipeline.immediate_mode==GL_TRIANGLES;
    if(resident)n-=n%3u; // trailing one/two captured vertices make no primitive
    if(!pipeline.failed && n){
        /* Compact the last partial SoA batch in place. Every source component
         * begins at or after its destination; ascending copies cannot clobber
         * a later component, including the non-power-of-two tails. */
        for(unsigned r=0;r<SR_REGISTERS*4;r++)if(pipeline.input_used[r/4])for(unsigned lane=0;lane<n;lane++)
            pipeline.inputs[r*n+lane]=pipeline.inputs[r*pipeline.immediate_batch+lane];
        (void)run_vertices(pipeline.immediate_mode,pipeline.immediate_count,n,
                            pipeline.immediate_previous,&pipeline.immediate_origin,resident);
        pipeline.immediate_count+=n;
    }
    if(!pipeline.failed && pipeline.immediate_mode==GL_LINE_LOOP && pipeline.immediate_count>=2)
        gl_raster_line(&pipeline.immediate_previous[(pipeline.immediate_count-1)&3u],&pipeline.immediate_origin);
    if(!pipeline.failed)gl_shader_gpu_flush();
    pipeline.immediate_pending=0;finish_draw();
}
