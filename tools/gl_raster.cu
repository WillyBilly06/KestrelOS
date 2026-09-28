/* Ordered, fixed-function application triangles. One GPU thread owns a pixel
 * across all commands, making depth/blend order deterministic without atomics.
 * Kept separate from gui_raster and the proven boot triangle shader.
 * No CPU pixel generation, CUDA texture objects, device allocations or shared
 * memory. The native dispatcher must validate/pin all buffers through its fence.
 */
#include "../include/kestrel/gpu3d.h"
#include <math.h>

static __device__ __forceinline__ float glgpu_clamp(float f) {
    return fminf(1.0f, fmaxf(0.0f, f));
}
static __device__ __forceinline__ bool glgpu_test(unsigned f, float a, float b) {
    switch(f) {
    case KG3D_NEVER: return false;
    case KG3D_LESS: return a < b;
    case KG3D_EQUAL: return a == b;
    case KG3D_LEQUAL: return a <= b;
    case KG3D_GREATER: return a > b;
    case KG3D_NOTEQUAL: return a != b;
    case KG3D_GEQUAL: return a >= b;
    default: return true;
    }
}
static __device__ __forceinline__ float glgpu_factor(unsigned f, float a) {
    /* XRGB destination has alpha one, independently of its unused upper byte. */
    switch(f) {
    case KG3D_ZERO: case KG3D_ONE_MINUS_DST_ALPHA: return 0;
    case KG3D_SRC_ALPHA: return a;
    case KG3D_ONE_MINUS_SRC_ALPHA: return 1-a;
    default: return 1;
    }
}
static __device__ __forceinline__ unsigned glgpu_pack(const float *c) {
    return ((unsigned)(glgpu_clamp(c[0])*255.0f+0.5f)<<16) |
           ((unsigned)(glgpu_clamp(c[1])*255.0f+0.5f)<<8) |
            (unsigned)(glgpu_clamp(c[2])*255.0f+0.5f);
}
static __device__ __forceinline__ int glgpu_wrap(int x, unsigned n, unsigned mode) {
    if(mode==KG3D_REPEAT) { x%=(int)n; return x<0 ? x+(int)n : x; }
    return x<0 ? 0 : (x>=(int)n ? (int)n-1 : x);
}
static __device__ __forceinline__ unsigned glgpu_texel(
    const unsigned *texture, unsigned w, unsigned h, unsigned pitch,
    int x, int y, unsigned ws, unsigned wt) {
    return texture[(unsigned long long)glgpu_wrap(y,h,wt)*pitch+glgpu_wrap(x,w,ws)];
}
static __device__ __forceinline__ void glgpu_sample(
    const unsigned *texture, unsigned w, unsigned h, unsigned pitch,
    float s, float t, const kg3d_command_t &c, float *rgba) {
    /* Reduce before integer conversion: hostile/large coordinates cannot
     * overflow an index. Clamp-to-edge must clamp to [0,1], not wrap first. */
    s=c.wrap_s==KG3D_REPEAT ? s-floorf(s) : glgpu_clamp(s);
    t=c.wrap_t==KG3D_REPEAT ? t-floorf(t) : glgpu_clamp(t);
    float u=s*w-0.5f, v=(1.0f-t)*h-0.5f;
    int x=(int)floorf(u), y=(int)floorf(v);
    float fx=u-x, fy=v-y;
    unsigned p00, p10, p01, p11;
    if(c.filter==KG3D_NEAREST) {
        p00=glgpu_texel(texture,w,h,pitch,(int)floorf(u+0.5f),(int)floorf(v+0.5f),c.wrap_s,c.wrap_t);
        p10=p01=p11=p00; fx=fy=0;
    } else {
        p00=glgpu_texel(texture,w,h,pitch,x,y,c.wrap_s,c.wrap_t);
        p10=glgpu_texel(texture,w,h,pitch,x+1,y,c.wrap_s,c.wrap_t);
        p01=glgpu_texel(texture,w,h,pitch,x,y+1,c.wrap_s,c.wrap_t);
        p11=glgpu_texel(texture,w,h,pitch,x+1,y+1,c.wrap_s,c.wrap_t);
    }
    #pragma unroll
    for(unsigned k=0;k<4;k++) {
        unsigned shift=k==3 ? 24 : 16-k*8;
        float a=(float)((p00>>shift)&255), b=(float)((p10>>shift)&255);
        float d=(float)((p01>>shift)&255), e=(float)((p11>>shift)&255);
        rgba[k]=((a+(b-a)*fx)*(1-fy)+(d+(e-d)*fx)*fy)*(1.0f/255.0f);
    }
}
static __device__ __forceinline__ long long glgpu_edge(
    long long ax,long long ay,long long bx,long long by,long long x,long long y) {
    return (bx-ax)*(y-ay)-(by-ay)*(x-ax);
}
static __device__ __forceinline__ bool glgpu_top_left(
    long long ax,long long ay,long long bx,long long by) {
    return ay==by ? bx<ax : by<ay;
}

static __device__ __forceinline__ void glgpu_fragment(
    const kg3d_command_t &c, float *rgba, float z,
    unsigned &pixel,float &stored,bool &colour_dirty,bool &depth_dirty) {
    if(!isfinite(z) || z<0 || z>1 ||
       ((c.flags&KG3D_DEPTH_TEST) && !glgpu_test(c.depth_func,z,stored)))return;
    if(!isfinite(rgba[0]) || !isfinite(rgba[1]) || !isfinite(rgba[2]) || !isfinite(rgba[3]))return;
    if((c.flags&KG3D_ALPHA_TEST) && !glgpu_test(c.alpha_func,rgba[3],c.alpha_ref))return;
    if(c.flags&KG3D_BLEND) {
        float sf=glgpu_factor(c.blend_src,glgpu_clamp(rgba[3]));
        float df=glgpu_factor(c.blend_dst,glgpu_clamp(rgba[3]));
        #pragma unroll
        for(unsigned k=0;k<3;k++)
            rgba[k]=rgba[k]*sf+((pixel>>(16-k*8))&255)*(1.0f/255.0f)*df;
    }
    pixel=glgpu_pack(rgba);colour_dirty=true;
    if(c.flags&KG3D_DEPTH_WRITE) { stored=z;depth_dirty=true; }
}

#ifndef GLGPU_HELPERS_ONLY
extern "C" __global__ void gl_raster(
    unsigned *dst, float *depth, const unsigned *texture,
    const kg3d_command_t *commands, unsigned long long texture_bytes,
    unsigned width,unsigned height,unsigned pitch,unsigned depth_pitch,
    unsigned tex_w,unsigned tex_h,unsigned tex_pitch,unsigned count,
    unsigned clip_x,unsigned clip_y,unsigned clip_w,unsigned clip_h) {
    unsigned long long tx=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;
    unsigned long long ty=(unsigned long long)blockIdx.y*blockDim.y+threadIdx.y;
    unsigned long long x=clip_x+tx, y=clip_y+ty;
    if(tx>=clip_w || ty>=clip_h || x>=width || y>=height ||
       width>32768u || height>32768u || pitch<width ||
       !dst || !commands || !count || count>KG3D_MAX_COMMANDS ||
       (depth && depth_pitch<width))return;
    bool texture_valid=texture && tex_w && tex_h && tex_w<=32768u && tex_h<=32768u &&
                       tex_pitch>=tex_w &&
                       ((unsigned long long)(tex_h-1)*tex_pitch+tex_w)<=texture_bytes/4;
    unsigned long long index=y*pitch+x, zi=y*depth_pitch+x;
    unsigned pixel=dst[index];
    float stored=depth ? depth[zi] : 1.0f;
    bool colour_dirty=false, depth_dirty=false;
    for(unsigned i=0;i<count;i++) {
        const kg3d_command_t &c=commands[i];
        long long rx=(long long)x-c.x, ry=(long long)y-c.y;
        if(c.width<=0 || c.height<=0 || rx<0 || ry<0 || rx>=c.width || ry>=c.height ||
            (c.flags & ~KG3D_FLAGS) || c.depth_func>KG3D_ALWAYS || c.alpha_func>KG3D_ALWAYS ||
           c.blend_src>KG3D_ONE_MINUS_DST_ALPHA || c.blend_dst>KG3D_ONE_MINUS_DST_ALPHA ||
           c.tex_env>KG3D_DECAL || c.wrap_s>KG3D_CLAMP || c.wrap_t>KG3D_CLAMP ||
            c.filter>KG3D_LINEAR || c.reserved[0] || c.reserved[1] || c.reserved[2] ||
            ((c.flags&(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_CLEAR_DEPTH)) && !depth))continue;
        unsigned primitive=c.flags&(KG3D_LINE|KG3D_POINT);
        if(primitive && (primitive==(KG3D_LINE|KG3D_POINT) ||
           (c.flags&(KG3D_TEXTURE|KG3D_CLEAR_COLOUR|KG3D_CLEAR_DEPTH))))continue;
        if(c.flags&(KG3D_CLEAR_COLOUR|KG3D_CLEAR_DEPTH)) {
            if(c.flags&KG3D_CLEAR_COLOUR) { pixel=glgpu_pack(c.v[0].rgba);colour_dirty=true; }
            if((c.flags&KG3D_CLEAR_DEPTH) && isfinite(c.v[0].z)) {
                stored=glgpu_clamp(c.v[0].z);depth_dirty=true;
            }
            continue;
        }
        bool valid=isfinite(c.depth_bias) && isfinite(c.alpha_ref);
        #pragma unroll
        for(unsigned j=0;j<3;j++) {
            valid=valid && fabsf(c.v[j].x)<=1048576.0f && fabsf(c.v[j].y)<=1048576.0f &&
                  isfinite(c.v[j].z) && c.v[j].inv_w>0.0f && isfinite(c.v[j].inv_w);
        }
        if(!valid || ((c.flags&KG3D_TEXTURE) && !texture_valid))continue;
        const kg3d_vertex_t *a=&c.v[0], *b=&c.v[1], *d=&c.v[2];
        if(primitive) {
            if(primitive==KG3D_POINT)b=a;
            float dx=b->x-a->x, dy=b->y-a->y;
            float extent=fmaxf(fabsf(dx),fabsf(dy));
            if(primitive==KG3D_LINE && extent>=8193.0f)continue;
            int steps=(int)extent;if(steps<1)steps=1;
            int first=0,last=primitive==KG3D_POINT?0:steps;
            if(primitive==KG3D_LINE && extent>=1) {
                /* The dominant coordinate advances by >=1 pixel per step.
                 * Invert it to locate this pixel's few candidate steps, then
                 * reproduce the exact forward DDA/truncation test. ±2 covers
                 * half-pixel inversion and float32 rounding at bounded coords.
                 * No CPU step loop and no conflicting writes between threads. */
                bool horizontal=fabsf(dx)>=fabsf(dy);
                float delta=horizontal?dx:dy,origin=horizontal?a->x:a->y;
                float position=(float)(horizontal?x:y)+0.5f;
                int at=(int)floorf((position-origin)*steps/delta);
                first=at-2;if(first<0)first=0;
                last=at+3;if(last>steps)last=steps;
            }
            for(int step=first;step<=last;step++) {
                float t=primitive==KG3D_POINT?0.0f:__fdiv_rn((float)step,(float)steps);
                float sx=__fadd_rn(a->x,__fmul_rn(dx,t)),sy=__fadd_rn(a->y,__fmul_rn(dy,t));
                if((int)sx!=(int)x || (int)sy!=(int)y)continue;
                float iw=__fadd_rn(a->inv_w,__fmul_rn(b->inv_w-a->inv_w,t));
                if(!(iw>0) || !isfinite(iw))continue;
                float w=1.0f/iw,rgba[4];
                #pragma unroll
                for(unsigned k=0;k<4;k++)
                    rgba[k]=__fadd_rn(a->rgba[k],__fmul_rn(b->rgba[k]-a->rgba[k],t))*w;
                float z=__fadd_rn(a->z,__fmul_rn(b->z-a->z,t));
                glgpu_fragment(c,rgba,z,pixel,stored,colour_dirty,depth_dirty);
            }
            continue;
        }
        long long ax=__float2ll_rn(a->x*16), ay=__float2ll_rn(a->y*16);
        long long bx=__float2ll_rn(b->x*16), by=__float2ll_rn(b->y*16);
        long long dx=__float2ll_rn(d->x*16), dy=__float2ll_rn(d->y*16);
        long long area=glgpu_edge(ax,ay,bx,by,dx,dy);
        if(!area)continue;
        if(area<0) {
            const kg3d_vertex_t *p=b;b=d;d=p;
            long long q=bx;bx=dx;dx=q;q=by;by=dy;dy=q;area=-area;
        }
        long long px=(long long)x*16+8, py=(long long)y*16+8;
        long long e0=glgpu_edge(bx,by,dx,dy,px,py);
        long long e1=glgpu_edge(dx,dy,ax,ay,px,py);
        long long e2=glgpu_edge(ax,ay,bx,by,px,py);
        if(e0<0 || (!e0 && !glgpu_top_left(bx,by,dx,dy)) ||
           e1<0 || (!e1 && !glgpu_top_left(dx,dy,ax,ay)) ||
           e2<0 || (!e2 && !glgpu_top_left(ax,ay,bx,by)))continue;
        float inv_area=1.0f/(float)area;
        float l1=(float)e1*inv_area, l2=(float)e2*inv_area;
        /* Difference form preserves constant attributes exactly. Summing three
         * rounded barycentric weights can break EQUAL/LESS at uniform depth. */
        float z=a->z+(b->z-a->z)*l1+(d->z-a->z)*l2+c.depth_bias;
        if(!isfinite(z) || z<0 || z>1 ||
           ((c.flags&KG3D_DEPTH_TEST) && !glgpu_test(c.depth_func,z,stored)))continue;
        float iw=a->inv_w+(b->inv_w-a->inv_w)*l1+(d->inv_w-a->inv_w)*l2;
        if(!(iw>0) || !isfinite(iw))continue;
        float w=1.0f/iw, rgba[4];
        #pragma unroll
        for(unsigned k=0;k<4;k++)
            rgba[k]=(a->rgba[k]+(b->rgba[k]-a->rgba[k])*l1+(d->rgba[k]-a->rgba[k])*l2)*w;
        if(c.flags&KG3D_TEXTURE) {
            float s=(a->uv[0]+(b->uv[0]-a->uv[0])*l1+(d->uv[0]-a->uv[0])*l2)*w;
            float t=(a->uv[1]+(b->uv[1]-a->uv[1])*l1+(d->uv[1]-a->uv[1])*l2)*w, texel[4];
            if(!isfinite(s) || !isfinite(t))continue;
            glgpu_sample(texture,tex_w,tex_h,tex_pitch,s,t,c,texel);
            #pragma unroll
            for(unsigned k=0;k<4;k++) {
                if(c.tex_env==KG3D_REPLACE)rgba[k]=texel[k];
                else if(c.tex_env==KG3D_DECAL) {
                    if(k<3)rgba[k]=rgba[k]*(1-texel[3])+texel[k]*texel[3];
                } else rgba[k]*=texel[k];
            }
        }
        glgpu_fragment(c,rgba,z,pixel,stored,colour_dirty,depth_dirty);
    }
    if(colour_dirty)dst[index]=pixel;
    if(depth_dirty)depth[zi]=stored;
}
#endif
