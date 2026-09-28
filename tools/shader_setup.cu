/* Triangle-list assembly/clipping on GPU. No automatic arrays, implicit local
 * memory, shared memory, atomics, or dynamically allocated device storage. */
#include "../include/kestrel/shader_setup.h"
#include <math.h>

static __device__ __forceinline__ float setup_plane(const kshs_vertex_t *v,
                                                    unsigned plane,unsigned zero) {
    if(plane==0)return __fsub_rn(v->clip[3],1e-5f);
    if(plane==1)return __fadd_rn(v->clip[3],v->clip[0]);
    if(plane==2)return __fsub_rn(v->clip[3],v->clip[0]);
    if(plane==3)return __fadd_rn(v->clip[3],v->clip[1]);
    if(plane==4)return __fsub_rn(v->clip[3],v->clip[1]);
    if(plane==5)return zero?v->clip[2]:__fadd_rn(v->clip[3],v->clip[2]);
    return __fsub_rn(v->clip[3],v->clip[2]);
}
static __device__ __forceinline__ bool setup_copy(kshs_vertex_t *dst,
    const kshs_vertex_t *a,const kshs_vertex_t *b,float t,bool interpolate) {
    #pragma unroll 1
    for(unsigned k=0;k<4+SR_VARYING_N*4;k++){
        float av=k<4?a->clip[k]:a->varying[(k-4)/4][(k-4)%4];
        float value=av;
        if(interpolate){
            float bv=k<4?b->clip[k]:b->varying[(k-4)/4][(k-4)%4];
            value=__fadd_rn(av,__fmul_rn(__fsub_rn(bv,av),t));
        }
        if(!isfinite(value))return false;
        if(k<4)dst->clip[k]=value;
        else dst->varying[(k-4)/4][(k-4)%4]=value;
    }
    return true;
}
static __device__ __forceinline__ bool setup_valid(const kshs_config_t &c) {
    if(c.version!=KSHS_ABI || !c.lanes || c.lanes>KSH_MAX_LANES ||
       !c.triangle_count || c.triangle_count>KSHS_MAX_TRIANGLES ||
       c.first_vertex>c.lanes || c.triangle_count>(c.lanes-c.first_vertex)/3u ||
       c.varying_count>SR_VARYING_N || (c.flat_varying_mask&~((1u<<c.varying_count)-1u)) ||
       !c.framebuffer_width || !c.framebuffer_height ||
       c.framebuffer_width>32768 || c.framebuffer_height>32768 ||
       c.viewport_w<=0 || c.viewport_h<=0 || c.clip_w<=0 || c.clip_h<=0 ||
       c.clip_x<0 || c.clip_y<0 || (unsigned)c.clip_x>=c.framebuffer_width ||
       (unsigned)c.clip_y>=c.framebuffer_height ||
       (unsigned)c.clip_w>c.framebuffer_width-(unsigned)c.clip_x ||
       (unsigned)c.clip_h>c.framebuffer_height-(unsigned)c.clip_y ||
       c.clip_x<c.viewport_x || c.clip_y<c.viewport_y ||
       (long long)c.clip_x+c.clip_w>(long long)c.viewport_x+c.viewport_w ||
       (long long)c.clip_y+c.clip_h>(long long)c.viewport_y+c.viewport_h ||
       c.clip_zero_to_one>1 || c.clip_y_down>1 || c.cull_enable>1 || c.front_ccw>1 ||
       c.cull_face>KSHS_CULL_BOTH || !isfinite(c.depth_near) || !isfinite(c.depth_far) ||
       c.depth_near<0 || c.depth_near>1 || c.depth_far<0 || c.depth_far>1 ||
       c.reserved0 || c.reserved2 || c.reserved3 || c.reserved4[0] || c.reserved4[1])return false;
    const kg3d_command_t &s=c.state;
    return !(s.flags&~(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_ALPHA_TEST|KG3D_BLEND)) &&
        s.depth_func<=KG3D_ALWAYS && s.alpha_func<=KG3D_ALWAYS &&
        s.blend_src<=KG3D_ONE_MINUS_DST_ALPHA && s.blend_dst<=KG3D_ONE_MINUS_DST_ALPHA &&
        s.tex_env<=KG3D_DECAL && s.wrap_s<=KG3D_CLAMP && s.wrap_t<=KG3D_CLAMP &&
        s.filter<=KG3D_LINEAR && isfinite(s.depth_bias) && isfinite(s.alpha_ref) &&
        !s.reserved[0] && !s.reserved[1] && !s.reserved[2];
}

extern "C" __global__ void shader_setup(
    const float *registers,const ksh_status_t *vertex_status,kshr_command_t *commands,
    kshs_primitive_t *results,kshs_vertex_t *scratch,const kshs_config_t *config,
    unsigned long long register_bytes,unsigned long long vertex_status_bytes,
    unsigned long long command_bytes,unsigned long long result_bytes,
    unsigned long long scratch_bytes,unsigned long long config_bytes) {
    unsigned long long id=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;
    if(!config || config_bytes<sizeof(*config))return;
    const kshs_config_t &c=*config;
    if(id>=c.triangle_count || id>=KSHS_MAX_TRIANGLES || !results ||
       result_bytes<(id+1u)*sizeof(*results))return;
    kshs_primitive_t &result=results[id];
    result.count=0;result.result=KSH_BAD_PROGRAM;
    #pragma unroll 1
    for(unsigned i=0;i<KSHS_OUTPUT_TRIANGLES;i++){
        result.bounds[i].x=result.bounds[i].y=0;
        result.bounds[i].width=result.bounds[i].height=0;
    }
    if(!setup_valid(c))return;
    result.result=KSH_BAD_RESOURCE;
    if(!registers || !vertex_status || !commands || !scratch ||
       register_bytes<KSHS_REGISTER_BYTES(c.lanes) ||
       vertex_status_bytes<KSHS_VERTEX_STATUS_BYTES(c.lanes) ||
       command_bytes<KSHS_COMMAND_BYTES(c.triangle_count) ||
       result_bytes<KSHS_RESULT_BYTES(c.triangle_count) ||
       scratch_bytes<KSHS_SCRATCH_BYTES(c.triangle_count))return;
    /* Invocation zero gates the entire vertex job, including lanes outside
     * this subrange. Other invocations check their own three vertices before
     * consuming them. No in-grid barrier is assumed: after the setup fence,
     * the consumer must reject ALL output unless EVERY primitive completed. */
    unsigned first_lane=id?c.first_vertex+(unsigned)id*3u:0u;
    unsigned end_lane=id?first_lane+3u:c.lanes;
    #pragma unroll 1
    for(unsigned lane=first_lane;lane<end_lane;lane++)
        if(vertex_status[lane].result!=KSH_COMPLETE || !vertex_status[lane].executed ||
           vertex_status[lane].executed>KSH_MAX_STEPS)return;
    result.result=KSH_BAD_PROGRAM;
    kshs_vertex_t *a=scratch+id*(2u*KSHS_CLIP_VERTICES);
    kshs_vertex_t *b=a+KSHS_CLIP_VERTICES;
    #pragma unroll 1
    for(unsigned v=0;v<3;v++){
        unsigned lane=c.first_vertex+(unsigned)id*3u+v;
        #pragma unroll 1
        for(unsigned k=0;k<4+SR_VARYING_N*4;k++){
            float value=0;
            if(k<4)value=registers[(SR_POSITION*4u+k)*c.lanes+lane];
            else if((k-4u)/4u<c.varying_count) {
                value=registers[(SR_VARYING*4u+k-4u)*c.lanes+lane];
                /* Preserve validation of every original active input even
                 * when flat shading will replace it. The provoking vertex
                 * belongs to the original triangle, not the clipped polygon. */
                if(!isfinite(value))return;
                if(c.flat_varying_mask&(1u<<((k-4u)/4u))) {
                    unsigned provoking=c.first_vertex+(unsigned)id*3u+2u;
                    value=registers[(SR_VARYING*4u+k-4u)*c.lanes+provoking];
                }
            }
            if(!isfinite(value))return;
            if(k<4)a[v].clip[k]=value;
            else a[v].varying[(k-4u)/4u][(k-4u)%4u]=value;
        }
    }
    unsigned count=3;
    #pragma unroll 1
    for(unsigned plane=0;plane<7 && count;plane++){
        unsigned next_count=0;
        #pragma unroll 1
        for(unsigned i=0;i<count;i++){
            const kshs_vertex_t *cur=a+i,*next=a+(i+1u==count?0u:i+1u);
            float dc=setup_plane(cur,plane,c.clip_zero_to_one);
            float dn=setup_plane(next,plane,c.clip_zero_to_one);
            if(!isfinite(dc)||!isfinite(dn))return;
            bool ci=dc>=0,ni=dn>=0;
            if(ci){
                if(next_count>=KSHS_CLIP_VERTICES || !setup_copy(b+next_count,cur,cur,0,false))return;
                next_count++;
            }
            /* An endpoint exactly on the plane is already emitted by the
             * ordinary inside branch. Emitting its t=0/t=1 intersection too
             * creates duplicate vertices and violates the convex 3+planes
             * bound at frustum corners. Removing that zero-length edge does
             * not remove a covered triangle or alter its varying values. */
            if(ci!=ni && dc!=0.0f && dn!=0.0f){
                float denominator=__fsub_rn(dc,dn);
                if(!isfinite(denominator)||denominator==0)return;
                float t=__fdiv_rn(dc,denominator);
                if(!isfinite(t)||t<0||t>1 || next_count>=KSHS_CLIP_VERTICES ||
                   !setup_copy(b+next_count,cur,next,t,true))return;
                next_count++;
            }
        }
        if(next_count>3u+plane+1u)return; // convex clip bound; never truncate
        count=next_count;kshs_vertex_t *old=a;a=b;b=old;
    }
    if(count>2u+KSHS_OUTPUT_TRIANGLES)return;
    unsigned emitted=0;
    #pragma unroll 1
    for(unsigned i=1;i+1u<count;i++){
        if(emitted>=KSHS_OUTPUT_TRIANGLES)return;
        kshr_command_t &out=commands[id*KSHS_OUTPUT_TRIANGLES+emitted];
        #pragma unroll 1
        for(unsigned v=0;v<3;v++){
            const kshs_vertex_t &in=a[v==0?0u:v==1?i:i+1u];
            float w=in.clip[3];
            if(!(w>0)||!isfinite(w))return;
            if(fabsf(w)<1e-7f)w=1e-7f;
            float iw=__fdiv_rn(1.0f,w);
            float nx=__fmul_rn(in.clip[0],iw),ny=__fmul_rn(in.clip[1],iw);
            float nz=__fmul_rn(in.clip[2],iw);
            float x=__fadd_rn((float)c.viewport_x,__fmul_rn(__fadd_rn(__fmul_rn(nx,0.5f),0.5f),(float)c.viewport_w));
            float y=__fadd_rn((float)c.viewport_y,__fmul_rn(__fadd_rn(__fmul_rn(c.clip_y_down?ny:-ny,0.5f),0.5f),(float)c.viewport_h));
            float z01=c.clip_zero_to_one?nz:__fadd_rn(__fmul_rn(nz,0.5f),0.5f);
            float z=__fadd_rn(c.depth_near,__fmul_rn(z01,__fsub_rn(c.depth_far,c.depth_near)));
            if(!(fabsf(x)<=1048576.0f)||!(fabsf(y)<=1048576.0f)||!isfinite(z)||!(iw>0)||!isfinite(iw))return;
            out.raster.v[v].x=x;out.raster.v[v].y=y;out.raster.v[v].z=z;out.raster.v[v].inv_w=iw;
            #pragma unroll
            for(unsigned k=0;k<4;k++)out.raster.v[v].rgba[k]=iw;
            out.raster.v[v].uv[0]=out.raster.v[v].uv[1]=0;
            #pragma unroll 1
            for(unsigned r=0;r<SR_VARYING_N;r++)for(unsigned k=0;k<4;k++){
                float value=__fmul_rn(in.varying[r][k],iw);
                if(!isfinite(value))return;
                out.varying[v][r][k]=value;
            }
        }
        const kg3d_vertex_t &p=out.raster.v[0],&q=out.raster.v[1],&r=out.raster.v[2];
        float area=__fsub_rn(__fmul_rn(__fsub_rn(q.x,p.x),__fsub_rn(r.y,p.y)),
                            __fmul_rn(__fsub_rn(q.y,p.y),__fsub_rn(r.x,p.x)));
        if(!isfinite(area))return;
        bool front=c.front_ccw?area<0:area>0;
        if(c.cull_enable && (c.cull_face==KSHS_CULL_BOTH ||
           (c.cull_face==KSHS_CULL_BACK&&!front) || (c.cull_face==KSHS_CULL_FRONT&&front)))continue;
        int left=(int)floorf(__fsub_rn(fminf(p.x,fminf(q.x,r.x)),0.0625f));
        int top=(int)floorf(__fsub_rn(fminf(p.y,fminf(q.y,r.y)),0.0625f));
        int right=(int)ceilf(__fadd_rn(fmaxf(p.x,fmaxf(q.x,r.x)),0.0625f));
        int bottom=(int)ceilf(__fadd_rn(fmaxf(p.y,fmaxf(q.y,r.y)),0.0625f));
        if(left<c.clip_x)left=c.clip_x;if(top<c.clip_y)top=c.clip_y;
        if(right>c.clip_x+c.clip_w)right=c.clip_x+c.clip_w;
        if(bottom>c.clip_y+c.clip_h)bottom=c.clip_y+c.clip_h;
        if(left>=right||top>=bottom)continue;
        out.raster.x=left;out.raster.y=top;out.raster.width=right-left;out.raster.height=bottom-top;
        out.raster.flags=c.state.flags;out.raster.depth_func=c.state.depth_func;
        out.raster.alpha_func=c.state.alpha_func;out.raster.blend_src=c.state.blend_src;
        out.raster.blend_dst=c.state.blend_dst;out.raster.tex_env=c.state.tex_env;
        out.raster.wrap_s=c.state.wrap_s;out.raster.wrap_t=c.state.wrap_t;out.raster.filter=c.state.filter;
        out.raster.depth_bias=c.state.depth_bias;out.raster.alpha_ref=c.state.alpha_ref;
        out.raster.reserved[0]=out.raster.reserved[1]=out.raster.reserved[2]=0;
        result.bounds[emitted].x=left;result.bounds[emitted].y=top;
        result.bounds[emitted].width=right-left;result.bounds[emitted].height=bottom-top;
        emitted++;
    }
    /* No consumer may use partial records. Fence required before inspection;
     * publish the count only when every emitted triangle was fully produced. */
    __threadfence();result.count=emitted;result.result=KSH_COMPLETE;
}
