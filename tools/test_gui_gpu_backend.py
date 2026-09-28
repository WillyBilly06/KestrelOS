#!/usr/bin/env python3
"""Execute the actual desktop batching/backend against modeled surface syscalls.
Checks pixel results, CPU-shadow non-use, ordering, sources and failure handling.
The separate CUDA test executes the shader on the real host GPU.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import run_test, function

ROOT=Path(__file__).resolve().parents[1]
GPU=(ROOT/'user/libgui/gpu.c').read_text()
DRAW=(ROOT/'user/libgui/draw.c').read_text()

def main():
    c=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint32_t colour_t;
typedef struct {colour_t *pixels;int width,height,stride;bool owns_pixels;
                struct gui_gpu_surface *gpu;} surface_t;
typedef struct {int x,y,w,h;} rect_t;
typedef struct {uint64_t colour,depth;unsigned pitch,depth_pitch;} gui_gpu_target_t;
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
bool gui_gpu_flush(void);
bool gui_gpu_attach(surface_t *);
surface_t *surface_create(int,int);
void surface_destroy(surface_t *);
colour_t colour_mix(colour_t,colour_t,int);
static void log_write(int level,const char *tag,const char *note){(void)level;(void)tag;puts(note);}
static unsigned sleeps;
static void sleep_ms(unsigned n){assert(n==1);sleeps++;}
#define SYS_GPU 100
#define GPUOP_SURFACE 9
static intptr_t syscall6(int,int,intptr_t,size_t,int,int,int);
static surface_t *clipped;
static rect_t test_clip;
static rect_t surface_clip(const surface_t *s){return s==clipped?test_clip:rect_make(0,0,s->width,s->height);}
static void surface_reset_clip(surface_t *s){if(s==clipped)clipped=NULL;}
static size_t target_alloc_bytes;
static unsigned target_alloc_calls,fail_target_alloc;
static void *target_calloc(size_t n,size_t size){
    target_alloc_calls++;target_alloc_bytes+=n*size;
    if(fail_target_alloc && !--fail_target_alloc)return NULL;
    return calloc(n,size);
}
#define calloc target_calloc
'''
    c+=DRAW[DRAW.index('bool rect_empty('):DRAW.index('/* ---------------------------------------------------------------- surfaces')]
    c+=re.sub(r'^#include "gui.h"\n','',GPU,flags=re.M).replace(
        '../../include/kestrel/gpu2d.h',(ROOT/'include/kestrel/gpu2d.h').as_posix()).replace(
        '../../include/kestrel/gpu3d.h',(ROOT/'include/kestrel/gpu3d.h').as_posix())
    constructor=DRAW.index('surface_t *surface_create(')
    c+=DRAW[constructor:DRAW.index('\n}',constructor)+2]+'\n#undef calloc\n'
    c+=r'''
#define RGB(r,g,b) (((r)<<16)|((g)<<8)|(b))
#define RGB_R(c) (((c)>>16)&255)
#define RGB_G(c) (((c)>>8)&255)
#define RGB_B(c) ((c)&255)
#define AA_ONE (1<<16)
static struct {colour_t window_shadow;} g_theme={0x080b12};
#define APP_ICON_SLOTS 48
typedef int icon_id;
typedef struct {colour_t *argb;int w,h;surface_t *gpu_image;} app_icon_bitmap_t;
typedef struct {uint8_t *a;int w,h;surface_t *gpu_image;} icon_mask_t;
static app_icon_bitmap_t app_icons[48];static icon_mask_t icon_masks[48];
static void text_runs_covered(const surface_t *s,rect_t r){(void)s;(void)r;}
void gui_line(surface_t *,int,int,int,int,colour_t);
void gui_blit(surface_t *,const surface_t *,int,int);
colour_t colour_mix(colour_t,colour_t,int);
static void blit_fill_row(colour_t *p,int n,colour_t c){while(n-->0)*p++=c;}
static void blit_blend_row(colour_t *p,int n,colour_t c,int a){while(n-->0){*p=colour_mix(*p,c,a);p++;}}
static void blit_copy_row(colour_t *p,const colour_t *s,int n){memcpy(p,s,(size_t)n*4);}
typedef struct {const uint8_t *glyphs,*widths;int cell_w,cell_h,stride,bytes;bool coverage;int draw_scale;} font_desc_t;
'''
    for name in ['colour_mix','gui_pixel','gui_fill','gui_clear','gui_hline','gui_vline',
                  'gui_frame','blend_at','gui_blend_pixel','gui_line_aa','gui_line','draw_glyph',
                 'gui_gradient_v','gui_gradient_h','gui_blend_rect','gui_blit_rect','gui_blit',
                  'gui_shadow','shadow_falloff','gui_soft_shadow','corner_inside','gui_round_rect','gui_round_frame','corner_coverage','gui_round_rect_aa',
                  'gui_round_frame_aa','gui_round_top','gui_round_gradient_top',
                  'gui_register_app_icon','gui_register_icon_mask','lerp_argb',
                  'app_icon_blit','draw_icon_mask']:
        m=re.search(r'^(?:static )?\w+ '+name+r'\([^;]*?\)\s*\{',DRAW,re.M)
        assert m,name
        c+=DRAW[m.start():DRAW.index('\n}',m.end())+2]+'\n'
    # Exact former per-pixel path: disable only the new GPU icon fast path.
    m=re.search(r'^static void app_icon_blit\([^;]*?\)\s*\{',DRAW,re.M)
    legacy=DRAW[m.start():DRAW.index('\n}',m.end())+2]
    c+=re.sub(r'^.*if\(gui_gpu_icon.*\n','',legacy,flags=re.M).replace(
        'void app_icon_blit(', 'void legacy_app_icon_blit(')+'\n'
    legacy_shadow=function(DRAW,'gui_soft_shadow')
    c+=re.sub(r'^.*if \(gui_gpu_shadow.*\n','',legacy_shadow,flags=re.M).replace(
        'void gui_soft_shadow(', 'void legacy_gui_soft_shadow(')+'\n'
    legacy_menu_shadow=function(DRAW,'gui_shadow')
    begin=legacy_menu_shadow.index('        if (s->gpu) {')
    end=legacy_menu_shadow.index('        rect_t clip = surface_clip(s);',begin)
    c+=(legacy_menu_shadow[:begin]+legacy_menu_shadow[end:]).replace(
        'void gui_shadow(', 'void legacy_gui_shadow(')+'\n'
    for name in ['gui_round_rect_aa','gui_round_frame_aa','gui_round_top','gui_round_gradient_top']:
        legacy=function(DRAW,name)
        c+=re.sub(r'^.*if\(gui_gpu_rounded.*\n','',legacy,flags=re.M).replace(
            'void '+name+'(', 'void legacy_'+name+'(')+'\n'
    c+=r'''
typedef struct {unsigned w,h,pitch;size_t bytes;unsigned char *data;} memory_t;
static memory_t memories[512];
static unsigned next_handle=1, creates,destroys,draws,uploads,presents,draws3d,requests;
static unsigned busy_count;
static bool fail_draw,fail_create;
static unsigned live_limit;
static int create_error;
static unsigned create_attempts;
static bool fail_destroy;
void surface_destroy(surface_t *s){if(s){gui_gpu_detach(s);free(s->pixels);free(s);}}
static uint32_t lerp(uint32_t a,uint32_t b,unsigned t){uint32_t p=0;
    for(unsigned s=0;s<32;s+=8)p|=(((((a>>s)&255)*(256-t)+((b>>s)&255)*t)>>8)<<s);return p;}
static uint32_t mix(uint32_t a,uint32_t b,unsigned v){
    if(!v)return a;if(v==255)return b;uint32_t p=0;
    for(unsigned s=0;s<24;s+=8)p|=((((a>>s)&255)*(255-v)+((b>>s)&255)*v)/255)<<s;
    return p;
}
static intptr_t syscall6(int sys,int op,intptr_t pointer,size_t size,int a,int b,int d){
    requests++;
    assert(sys==SYS_GPU && op==GPUOP_SURFACE && size==sizeof(kg2d_request_t));
    assert(!a&&!b&&!d);
    if(busy_count){busy_count--;return -EBUSY;}
    kg2d_request_t *r=(void*)pointer;assert(r->version==KG2D_ABI);
    if(r->operation==KG2D_CREATE){
        create_attempts++;
        if(create_error)return create_error;
        if(live_limit&&creates-destroys>=live_limit)return -ENOMEM;
        if(fail_create)return -ENOMEM;
        assert(next_handle<512);unsigned h=next_handle++;
        memory_t *m=&memories[h];m->w=r->width;m->h=r->height;m->pitch=(r->width*4+255)&~255u;
        m->bytes=((size_t)m->pitch*m->h+65535)&~65535ull;m->data=calloc(1,m->bytes);assert(m->data);
        r->handle=h;r->pitch=m->pitch;r->bytes=m->bytes;creates++;return 0;
    }
    assert(r->handle<next_handle);memory_t *m=&memories[r->handle];assert(m->data);
    switch(r->operation){
    case KG2D_DESTROY:if(fail_destroy)return -EIO;free(m->data);m->data=NULL;destroys++;return 0;
    case KG2D_UPLOAD:case KG2D_DOWNLOAD:
        assert(r->bytes && r->bytes<=KG2D_TRANSFER_MAX && !((r->offset|r->bytes)&3));
        assert(r->offset<=m->bytes && r->bytes<=m->bytes-r->offset);
        if(r->operation==KG2D_UPLOAD){memcpy(m->data+r->offset,(void*)(uintptr_t)r->data,r->bytes);uploads++;}
        else memcpy((void*)(uintptr_t)r->data,m->data+r->offset,r->bytes);
        return 0;
    case KG2D_PRESENT:assert(draws);presents++;return 0;
    case KG2D_DRAW3D:{
        if(fail_draw)return -EIO;
        assert(r->count && r->count<=64);
        assert(r->x+r->width<=m->w && r->y+r->height<=m->h);
        assert(r->offset && r->offset<next_handle && r->offset!=r->handle);
        memory_t *z=&memories[r->offset];
        assert(z->data && z->w==m->w && z->h==m->h);
        const kg3d_command_t *cmd=(void*)(uintptr_t)r->data;
        /* Model the clear-only cases here; the real shader is CUDA-tested. */
        uint64_t work=0;
        for(unsigned i=0;i<r->count;i++){
            assert(cmd[i].flags==KG3D_CLEAR_DEPTH || cmd[i].flags==(KG3D_LINE|KG3D_DEPTH_TEST));
            work+=(cmd[i].flags&KG3D_LINE)?KG3D_LINE_WORK:1;
        }
        assert((uint64_t)r->width*r->height*work<=KG3D_MAX_WORK);
        if(cmd[0].flags==KG3D_CLEAR_DEPTH)
            for(unsigned y=r->y;y<r->y+r->height;y++)for(unsigned x=r->x;x<r->x+r->width;x++)
                memcpy(z->data+(size_t)y*z->pitch+x*4,&cmd[0].v[0].z,4);
        draws3d++;return 0;
    }
    case KG2D_DRAW:{
        if(fail_draw)return -EIO;
        assert(r->count && r->count<=256 && r->source!=r->handle);
        assert(r->x+r->width<=m->w && r->y+r->height<=m->h);
        kg2d_command_t *cmd=(void*)(uintptr_t)r->data;
        for(unsigned i=0;i<r->count;i++){
            kg2d_command_t q=cmd[i];
            for(unsigned y=r->y;y<r->y+r->height;y++)for(unsigned x=r->x;x<r->x+r->width;x++){
                int64_t rx=(int64_t)x-q.x,ry=(int64_t)y-q.y;
                if(rx<0||ry<0||rx>=q.width||ry>=q.height)continue;
                uint32_t colour=q.colour0,alpha=q.opacity;
                if(q.op==KG2D_GRADIENT_H||q.op==KG2D_GRADIENT_V){
                    unsigned n=q.op==KG2D_GRADIENT_H?q.width:q.height;
                    colour=mix(q.colour0,q.colour1,n>1?(q.op==KG2D_GRADIENT_H?rx:ry)*255/(n-1):0);
                }
                if(q.op==KG2D_ROUNDED){
                    int radius=q.source_x,style=q.source_y;
                    int near_x=rx<q.width-1-rx?rx:q.width-1-rx;
                    int near_y=style!=2 && style!=3 && q.height-1-ry<ry?q.height-1-ry:ry;
                    int coverage=255;
                    if(near_x<radius && near_y<radius){
                        if(style>=4){
                            int dx=radius-near_x-1,dy=radius-near_y-1;
                            coverage=corner_inside(dx,dy,radius) &&
                                (style==4 || !near_x || !corner_inside(dx+1,dy,radius))?255:0;
                        }else{
                            coverage=corner_coverage(radius-near_x,radius-near_y,radius);
                            if(style==1){coverage-=corner_coverage(radius-near_x,radius-near_y,radius-1);if(coverage<=4)coverage=0;}
                            if(style==3)coverage=coverage?255:0;
                        }
                    }else if(style==1 || style==5)coverage=rx==0||ry==0||rx==q.width-1||ry==q.height-1?255:0;
                    if(style==3)colour=colour_mix(q.colour0,q.colour1,q.height>1?ry*255/(q.height-1):0);
                    alpha=alpha*coverage/255;
                }
                if(q.op==KG2D_LINE){
                    unsigned dx=q.width-1,dy=q.height-1;
                    uint64_t u=q.colour1&1?dx-rx:rx,v=q.colour1&2?dy-ry:ry;
                    if(dx>=dy){if(v!=(dx?(u*dy+(dx-1)/2)/dx:0))continue;}
                    else if(u!=(v*dx+(dy-1)/2)/dy)continue;
                }
                if(q.op==KG2D_LINE_AA){
                    bool steep=q.colour1&1;
                    unsigned major=(steep?q.height:q.width)-1,minor=(steep?q.width:q.height)-2;
                    uint64_t k=steep?ry:rx,v=steep?rx:ry,step=(uint64_t)minor*65536/major;
                    uint64_t fixed=q.colour1&2?(uint64_t)minor*65536-k*step:k*step;
                    unsigned lower=(fixed%65536)*255/65536;
                    alpha=alpha*(v==fixed/65536?255-lower:v==fixed/65536+1?lower:0)/255;
                }
                if(q.op==KG2D_GLYPH){
                    memory_t *src=&memories[r->source];assert(r->source&&src->data);
                    unsigned u=(uint64_t)rx*q.source_x/q.width,v=(uint64_t)ry*q.source_y/q.height;
                    size_t at=q.source_offset+(size_t)v*q.source_stride+(q.colour1?u/8:u);
                    assert(at<src->bytes);
                    unsigned coverage=q.colour1?(src->data[at]&(0x80>>(u%8))?255:0):src->data[at];
                    if(coverage>=254)coverage=255;alpha=alpha*coverage/255;
                }
                if(q.op==KG2D_SHADOW){
                    int spread=q.source_x,bx=q.x+spread,by=q.y+spread+1;
                    int bw=q.width-2*spread,bh=q.height-2*spread-2;
                    int dx=(int)x<bx?bx-x:(int)x>=bx+bw?x-(bx+bw)+1:0;
                    int dy=(int)y<by?by-y:(int)y>=by+bh?y-(by+bh)+1:0;
                    if(!dx && !dy)continue;
                    int distance=dx>dy?dx+dy/2:dy+dx/2;
                    if(dx&&dy)distance+=q.source_y/3;
                    alpha=shadow_falloff(distance,spread,alpha);
                }
                if(q.op>=KG2D_IMAGE && q.op<=KG2D_ARGB_BILINEAR){
                    memory_t *s=&memories[r->source];assert(r->source&&s->data);
                    if(q.op==KG2D_ARGB_BILINEAR){
                        unsigned qx=rx*(q.reserved[0]-1)*256/(q.width>1?q.width-1:1);
                        unsigned qy=ry*(q.reserved[1]-1)*256/(q.height>1?q.height-1:1);
                        unsigned fx=qx&255,fy=qy&255;rx=qx>>8;ry=qy>>8;
                        size_t off=q.source_offset+(q.source_y+ry)*q.source_stride+(q.source_x+rx)*4;
                        assert(off+(fy?q.source_stride:0)+(fx?4:0)+4<=s->bytes);
                        uint32_t p[4];memcpy(&p[0],s->data+off,4);memcpy(&p[1],s->data+off+(fx?4:0),4);
                        off+=fy?q.source_stride:0;memcpy(&p[2],s->data+off,4);memcpy(&p[3],s->data+off+(fx?4:0),4);
                        uint32_t argb=lerp(lerp(p[0],p[1],fx),lerp(p[2],p[3],fx),fy);
                        colour=(q.colour1?q.colour0:argb)&0xffffff;alpha=alpha*(argb>>24)/255;
                    } else {
                    if(q.op==KG2D_IMAGE_SCALED){rx=rx*q.reserved[0]/q.width;ry=ry*q.reserved[1]/q.height;}
                    unsigned bpp=q.op==KG2D_MASK_A8?1:4;
                    size_t off=q.source_offset+(q.source_y+ry)*q.source_stride+(q.source_x+rx)*bpp;
                    assert(off+bpp<=s->bytes);
                    if(bpp==4)memcpy(&colour,s->data+off,4);else alpha=alpha*s->data[off]/255;
                    }
                }
                uint32_t *pixel=(void*)(m->data+(size_t)y*m->pitch+x*4);
                *pixel=mix(*pixel,colour,alpha);
            }
        }
        draws++;return 0;
    }
    default:assert(false);return -EINVAL;
    }
}
static surface_t make_surface(int w,int h){
    surface_t s={.pixels=malloc((size_t)w*h*4),.width=w,.height=h,.stride=w,.owns_pixels=true};
    assert(s.pixels);for(int i=0;i<w*h;i++)s.pixels[i]=0xdeadbeef;
    assert(gui_gpu_attach(&s));return s;
}
static uint32_t read_gpu(surface_t *s,int x,int y){
    memory_t *m=&memories[s->gpu->handle];uint32_t p;memcpy(&p,m->data+(size_t)y*m->pitch+x*4,4);return p;
}
static void check(surface_t *s,const uint32_t *expected){
    assert(gui_gpu_flush());
    for(int y=0;y<s->height;y++)for(int x=0;x<s->width;x++){
        if(read_gpu(s,x,y)!=expected[y*s->width+x]){
            fprintf(stderr,"pixel %d,%d: %08x != %08x\n",x,y,read_gpu(s,x,y),expected[y*s->width+x]);abort();
        }
        assert(!s->pixels); /* no CPU widget raster/shadow allocation exists */
    }
}
static void paint_widgets(surface_t *s){
    gui_clear(s,0x082438);
    gui_gradient_v(s,rect_make(-9,-5,153,113),0x156ac1,0x91af02);
    gui_gradient_h(s,rect_make(3,9,81,37),0x751baa,0xf38764);
    gui_blend_rect(s,rect_make(19,17,43,21),0xc13890,123);
    gui_line_aa(s,2,8,123,93,0xf1f3f5);
    gui_round_rect_aa(s,rect_make(11,7,37,21),7,0x9321f1);
    gui_round_frame_aa(s,rect_make(17,37,54,31),9,0xbbee99);
    gui_round_top(s,rect_make(77,11,43,25),8,0x113355);
    gui_round_gradient_top(s,rect_make(61,46,54,40),11,0x203951,0xfecc22);
    gui_soft_shadow(s,rect_make(31,21,47,36),7,91,8);
}
int main(void){
    {
        /* Independent reference: original iterative CPU Bresenham, not the
         * shader's closed form. Cover every endpoint pair on a small grid. */
        surface_t *gpu=surface_create_target(32,24,true),*cpu=surface_create(32,24);
        assert(gpu&&cpu);unsigned cases=0;
        const int coords[]={-9,-1,0,1,8,15,16,23,31,39};
        for(unsigned aa=0;aa<2;aa++)for(unsigned a=0;a<10;a++)for(unsigned b=0;b<10;b++)
        for(unsigned d=0;d<10;d++)for(unsigned e=0;e<10;e++) {
            gui_clear(gpu,0x123456);gui_clear(cpu,0x123456);assert(gui_gpu_flush());
            unsigned before=requests;
            if(aa)gui_line_aa(gpu,coords[a],coords[b],coords[d],coords[e],0xfedcba);
            else gui_line(gpu,coords[a],coords[b],coords[d],coords[e],0xfedcba);
            assert(requests==before&&batch.count<=1&&!gpu->pixels);
            if(aa)gui_line_aa(cpu,coords[a],coords[b],coords[d],coords[e],0xfedcba);
            else gui_line(cpu,coords[a],coords[b],coords[d],coords[e],0xfedcba);
            check(gpu,cpu->pixels);cases++;
        }
        // Thousands of legacy pixel commands become one line record.
        surface_t *wide=surface_create_target(2048,8,true);assert(wide);
        unsigned before=draws;gui_line(wide,0,0,2047,7,0xaabbcc);
        assert(batch.count==1&&gui_gpu_flush()&&draws==before+1);
        before=requests;
        gui_line(wide,INT32_MAX,INT32_MIN,INT32_MAX,INT32_MIN,0);
        assert(requests==before&&!batch.failed); // invisible extreme point
        surface_destroy(wide);surface_destroy(gpu);surface_destroy(cpu);
        printf("PASS GPU line frontend: %u hard/AA endpoint pairs match iterative CPU rasterizers, one record for a 2048-pixel line, no CPU target\n",cases);
    }
    {
        // Fully clipped work must be a no-op before scratch allocation,
        // transfers, self-copy snapshots or flushing unrelated pending paint.
        surface_t *dst=surface_create_target(32,32,true);
        surface_t *src=surface_create_target(16,16,true);
        surface_t *cpu=surface_create(16,16);assert(dst&&src&&cpu&&!scratch_handle);
        gui_fill(dst,rect_make(0,0,2,2),0x123456);
        unsigned before=requests,pending=batch.count;assert(pending);
        assert(gui_gpu_mask(dst,NULL,2,2,2,1000,1000,0xffffff,255));
        assert(!scratch_handle&&requests==before&&batch.count==pending&&!batch.failed);
        // Invisible mask dimensions need no backing memory, even if they
        // would exceed the scratch budget when visible.
        assert(gui_gpu_mask(dst,NULL,4096,4096,4096,1000,1000,0,255));
        assert(gui_gpu_mask(dst,NULL,16,16,16,INT32_MAX-8,0,0,255));
        assert(requests==before&&batch.count==pending&&!batch.failed);
        rect_t from=rect_make(0,0,16,16),outside=rect_make(1000,1000,16,16);
        assert(gui_gpu_blit(dst,src,from,outside,255));
        assert(gui_gpu_blit(dst,cpu,from,outside,255));
        assert(gui_gpu_blit(dst,dst,from,outside,255));
        assert(gui_gpu_blit(cpu,src,from,outside,255));
        assert(gui_gpu_blit(dst,src,from,rect_make(INT32_MAX-8,0,16,16),255));
        assert(requests==before&&batch.count==pending&&!batch.failed);
        // Test the clip rectangle, not just the destination surface extent.
        clipped=dst;test_clip=rect_make(0,0,8,8);
        assert(gui_gpu_blit(dst,src,from,rect_make(16,16,16,16),255));
        assert(requests==before&&batch.count==pending);
        clipped=NULL;assert(gui_gpu_flush());
        assert(read_gpu(dst,0,0)==0x123456&&read_gpu(dst,2,2)==0);
        surface_destroy(dst);surface_destroy(src);surface_destroy(cpu);
        puts("PASS clipped GPU operations: no scratch/snapshot allocation, upload/readback or unrelated batch flush; queued pixels preserved");
    }
    assert(gui_gpu_validate());
    {
        surface_t *gpu=surface_create_target(96,32,true),*cpu=surface_create(96,32);assert(gpu&&cpu);
        const int widths[]={1,7,8,9,17};unsigned cases=0;
        for(unsigned coverage=0;coverage<2;coverage++)for(unsigned wi=0;wi<5;wi++)
        for(int scale=1;scale<=4;scale++)for(unsigned clip=0;clip<2;clip++) {
            int w=widths[wi],stride=(coverage?w:(w+7)/8)+2;
            uint8_t data[128];for(unsigned i=0;i<sizeof data;i++)data[i]=(uint8_t)(i*73+coverage*17);
            data[0]=254;data[1]=255;
            font_desc_t f={.glyphs=data,.cell_w=w,.cell_h=5,.stride=stride,
                .bytes=stride*5,.coverage=coverage,.draw_scale=scale};
            clipped=NULL;gui_clear(gpu,0x123456);gui_clear(cpu,0x123456);assert(gui_gpu_flush());
            if(clip){clipped=cpu;test_clip=rect_make(3,4,37,15);}
            draw_glyph(cpu,&f,-2,1,0,0xfedcba);
            if(clip)clipped=gpu;
            unsigned allocations=target_alloc_calls,before=requests;
            draw_glyph(gpu,&f,-2,1,0,0xfedcba);
            assert(target_alloc_calls==allocations&&requests==before&&batch.count<=1);
            if(batch.count)assert(batch.commands[0].op==KG2D_GLYPH&&scratch_used==(unsigned)f.bytes);
            clipped=NULL;check(gpu,cpu->pixels);cases++;
        }
        // Local font bytes can change immediately after enqueue: packed source
        // offsets retain each original glyph until the shared batch completes.
        uint8_t bits=0x80;gui_clear(gpu,0);assert(gui_gpu_flush());unsigned before=draws;
        gui_gpu_glyph(gpu,&bits,3,1,1,false,2,1,1,0xffffff);bits=0x20;
        gui_gpu_glyph(gpu,&bits,3,1,1,false,2,9,1,0xabcdef);
        assert(batch.count==2&&scratch_used==5&&gui_gpu_flush()&&draws==before+1);
        assert(read_gpu(gpu,1,1)==0xffffff&&read_gpu(gpu,5,1)==0);
        assert(read_gpu(gpu,9,1)==0&&read_gpu(gpu,13,1)==0xabcdef);
        before=requests;unsigned used=scratch_used;
        gui_gpu_glyph(gpu,NULL,INT32_MAX,1,1,false,4,INT32_MAX,0,0);
        assert(requests==before&&scratch_used==used&&!batch.failed);
        surface_destroy(gpu);surface_destroy(cpu);
        printf("PASS GPU glyph frontend: %u A8/packed-bitmap scale/padded-stride/clip cases match original draw_glyph, raw bytes only, no per-glyph allocation, immutable packed offsets\n",cases);
    }
    {
        surface_t *canvas=surface_create_target(80,80,true),*assets[8]={0};assert(canvas);
        uint32_t pixel=0xffabcdef;
        for(unsigned i=0;i<8;i++)gui_gpu_icon(canvas,&assets[i],&pixel,1,1,false,0,0,4,false,0);
        assert(gui_gpu_flush());
        // Queue a use of the oldest cache entry; retirement must precede its
        // eviction, even when the request that triggered reclamation is CREATE.
        kg2d_command_t copy={.op=KG2D_IMAGE_SCALED,.width=1,.height=1,.opacity=255,
            .source_stride=assets[0]->gpu->pitch,.reserved={1,1,0,0}};
        assert(enqueue(canvas,copy,assets[0]->gpu->handle));
        unsigned before=draws,dead=destroys;
        live_limit=creates-destroys;
        surface_t *window=surface_create_target(32,32,true);
        assert(window&&!assets[0]&&assets[1]&&destroys==dead+1&&draws==before+1&&!batch.failed);
        assert(read_gpu(canvas,0,0)==pixel); // opaque copy preserves the full word
        gui_gpu_target_t target;dead=destroys;
        assert(gui_gpu_target(window,true,&target));
        assert(!assets[1]&&assets[2]&&destroys==dead+1&&target.depth);
        uint64_t protect_a=assets[2]->gpu->handle,protect_b=assets[3]->gpu->handle;
        kg2d_request_t extra={.operation=KG2D_CREATE,.width=16,.height=16};
        assert(allocate_surface(&extra,protect_a,protect_b,false));
        assert(assets[2]&&assets[3]&&!assets[4]&&assets[5]);
        extra.operation=KG2D_DESTROY;assert(request(&extra));
        // Unknown/channel/argument errors must not be interpreted as pressure.
        for(int err=-EINVAL;err<=-EIO;err+=EINVAL-EIO) {
            create_error=err;dead=destroys;
            assert(!surface_create_target(32,32,true));
            assert(destroys==dead&&assets[2]&&!batch.failed);
        }
        // Persistent shortage has a bounded retry, and cannot evict protected
        // sources even after every other reclaimable icon has been released.
        create_error=-ENOMEM;unsigned attempts=create_attempts;dead=destroys;
        extra=(kg2d_request_t){.operation=KG2D_CREATE,.width=16,.height=16};
        assert(!allocate_surface(&extra,protect_a,protect_b,false));
        assert(create_attempts==attempts+4&&destroys==dead+3);
        assert(assets[2]&&assets[3]&&!assets[5]&&!assets[6]&&!assets[7]&&!batch.failed);
        attempts=create_attempts;dead=destroys;
        assert(!allocate_surface(&extra,0,0,false));
        assert(create_attempts==attempts+2&&destroys==dead+1&&!assets[2]&&assets[3]);
        // The remaining cache slot may be reused by another icon, but must not
        // be consumed by optional window growth. Subsequent icon painting works.
        create_error=0;live_limit=creates-destroys;
        attempts=create_attempts;dead=destroys;
        assert(!surface_create_target(16,16,true)&&assets[3]&&destroys==dead);
        assert(create_attempts==attempts+1&&!batch.failed);
        gui_gpu_icon(canvas,&assets[7],&pixel,1,1,false,8,8,4,false,0);
        assert(assets[7]&&!assets[3]&&destroys==dead+1&&!batch.failed);
        assert(gui_gpu_flush()&&read_gpu(canvas,8,8)==0xabcdef);
        create_error=0;live_limit=0;
        for(unsigned i=0;i<8;i++)surface_destroy(assets[i]);
        surface_destroy(window);surface_destroy(canvas);
        puts("PASS GPU slot pressure: LRU window/depth admission, pending-sample retirement, protected handles, bounded retry, non-pressure rejection, last-icon reserve and replacement");
    }
    {
        unsigned before_creates=creates,before_destroys=destroys;
        target_alloc_calls=0;target_alloc_bytes=0;
        surface_t *large=surface_create_target(7680,1440,true);
        assert(large && large->gpu && !large->pixels && large->owns_pixels);
        assert(large->width==7680 && large->height==1440 && large->stride==7680);
        assert(target_alloc_calls==2 && target_alloc_bytes==sizeof(surface_t)+sizeof(struct gui_gpu_surface));
        surface_destroy(large);
        assert(creates==before_creates+1 && destroys==before_destroys+1);
        for(unsigned failure=1;failure<=2;failure++) {
            before_creates=creates;fail_target_alloc=failure;
            assert(!surface_create_target(320,200,true));
            assert(!fail_target_alloc && creates==before_creates && !batch.failed);
        }
        fail_create=true;before_creates=creates;
        assert(!surface_create_target(320,200,true) && creates==before_creates && !batch.failed);
        fail_create=false;
        large=surface_create_target(320,200,true);assert(large && large->gpu && !large->pixels);
        surface_destroy(large);
        before_creates=creates;target_alloc_calls=0;
        assert(!surface_create_target(0,100,true) && !surface_create_target(100,-1,true));
        assert(!target_alloc_calls && creates==before_creates);
        surface_t *cpu=surface_create_target(37,29,false);
        assert(cpu && !cpu->gpu && cpu->pixels && creates==before_creates);
        for(unsigned i=0;i<37*29;i++)assert(!cpu->pixels[i]);surface_destroy(cpu);
        puts("PASS direct GPU target: 7680x1440 requires metadata only, no 44236800-byte CPU bitmap; CPU opt-in, three allocation failures, retry and release");
    }
    {
        surface_t *cpu=surface_create(80,64),*gpu=surface_create_target(80,64,true);
        assert(cpu && gpu && !gpu->pixels);unsigned cases=0;
        for(int depth=-1;depth<=12;depth++)for(int width=1;width<=51;width+=10)
        for(unsigned clip=0;clip<2;clip++) {
            clipped=NULL;gui_clear(cpu,0x314253);gui_clear(gpu,0x314253);
            assert(gui_gpu_flush());
            rect_t r=rect_make(-2,3,width,1+(depth+1)*3);
            if(clip){test_clip=rect_make(4,7,61,41);clipped=cpu;}
            gui_shadow(cpu,r,depth);if(clip)clipped=gpu;
            unsigned before=draws,up=uploads;gui_shadow(gpu,r,depth);
            assert(!gpu->pixels && uploads==up && batch.count<=(unsigned)(depth>0?depth*4:0));
            check(gpu,cpu->pixels);assert(draws<=before+1);cases++;
        }
        clipped=NULL;surface_destroy(cpu);surface_destroy(gpu);
        gpu=surface_create_target(320,240,true);assert(gpu && !gpu->pixels);
        rect_t menu=rect_make(25,25,240,160);
        gui_clear(gpu,0x314253);assert(gui_gpu_flush());unsigned before=draws;
        legacy_gui_shadow(gpu,menu,6);assert(gui_gpu_flush());unsigned old=draws-before;
        colour_t *expected=malloc(320*240*4);assert(expected);
        assert(gui_gpu_colour_readback(gpu,expected,320,320*240));
        gui_clear(gpu,0x314253);assert(gui_gpu_flush());before=draws;unsigned up=uploads;
        gui_shadow(gpu,menu,6);assert(batch.count==24 && draws==before && uploads==up);
        check(gpu,expected);assert(draws==before+1 && old>1);
        printf("PASS menu shadow: %u clipped/tiny/depth CPU-reference cases; 24 geometry records, %u former batches -> 1, exact double-blended corners, no source upload or CPU target\n",cases,old);
        free(expected);surface_destroy(gpu);
    }
    {
        surface_t *cpu=surface_create(40,32),*gpu=surface_create(40,32);assert(gui_gpu_attach(gpu));
        unsigned cases=0;
        for(unsigned style=0;style<6;style++)for(int radius=-1;radius<=16;radius++)
            for(int width=1;width<=35;width+=3){
                rect_t r=rect_make(-3,2,width,1+(radius+1)*2);
                gui_clear(cpu,0x314253);gui_clear(gpu,0x314253);
                switch(style){
                case 0:gui_round_rect_aa(cpu,r,radius,0x99abfe);gui_round_rect_aa(gpu,r,radius,0x99abfe);break;
                case 1:gui_round_frame_aa(cpu,r,radius,0x99abfe);gui_round_frame_aa(gpu,r,radius,0x99abfe);break;
                case 2:gui_round_top(cpu,r,radius,0x99abfe);gui_round_top(gpu,r,radius,0x99abfe);break;
                case 3:gui_round_gradient_top(cpu,r,radius,0x99abfe,0x112233);gui_round_gradient_top(gpu,r,radius,0x99abfe,0x112233);break;
                case 4:gui_round_rect(cpu,r,radius,0x99abfe);gui_round_rect(gpu,r,radius,0x99abfe);break;
                case 5:gui_round_frame(cpu,r,radius,0x99abfe);gui_round_frame(gpu,r,radius,0x99abfe);break;
                }
                check(gpu,cpu->pixels);cases++;
            }
        surface_destroy(cpu);surface_destroy(gpu);
        gpu=surface_create(640,520);assert(gui_gpu_attach(gpu));rect_t f=rect_make(35,35,570,450);
        gui_clear(gpu,0x314253);assert(gui_gpu_flush());unsigned before=draws;
        legacy_gui_round_rect_aa(gpu,f,28,0x99abfe);legacy_gui_round_frame_aa(gpu,f,28,0x112233);
        legacy_gui_round_gradient_top(gpu,rect_make(36,36,568,62),27,0x225588,0x113355);
        assert(gui_gpu_flush());unsigned old=draws-before;
        colour_t *expected=malloc(640*520*4);assert(expected&&gui_gpu_colour_readback(gpu,expected,640,640*520));
        gui_clear(gpu,0x314253);assert(gui_gpu_flush());before=draws;
        gui_round_rect_aa(gpu,f,28,0x99abfe);gui_round_frame_aa(gpu,f,28,0x112233);
        gui_round_gradient_top(gpu,rect_make(36,36,568,62),27,0x225588,0x113355);
        check(gpu,expected);assert(draws==before+1 && old>10);
        printf("PASS rounded GPU shapes: %u clipped/tiny/radius pixel cases; scale-2 frame %u former batches -> 1, identical pixels\n",cases,old);
        free(expected);surface_destroy(gpu);
    }
    {
        surface_t *gpu=surface_create(32,32);assert(gui_gpu_attach(gpu));
        uint8_t mask[4]={255,128,64,0};gui_clear(gpu,0);assert(gui_gpu_flush());
        unsigned before=draws,up=uploads;
        for(unsigned i=0;i<40;i++){
            gui_fill(gpu,rect_make(0,0,20,20),0x123456);
            gui_gpu_mask(gpu,mask,2,2,2,3,4,0xaabbcc,255);
        }
        assert(gui_gpu_flush() && draws==before+1 && uploads==up+1);
        assert(read_gpu(gpu,3,4)==0xaabbcc && read_gpu(gpu,4,4)==colour_mix(0x123456,0xaabbcc,128));
        puts("PASS mixed source-independent fills + glyph masks: 80 ordered commands in one draw and one mask upload");
        surface_destroy(gpu);
    }
    {
        surface_t *canvas=surface_create(80,80),*cache=NULL;assert(gui_gpu_attach(canvas));
        uint32_t icon[4]={0x00123456,0xffaabbcc,0x7f112233,0xffffffff};
        unsigned initial_uploads=uploads,initial_draws=draws;
        for(unsigned i=0;i<100;i++)gui_gpu_icon(canvas,&cache,icon,2,2,false,3,7,65,false,0);
        assert(gui_gpu_flush() && uploads==initial_uploads+1);
        assert(draws-initial_draws<10); /* Previously >1600 batches for these draws. */
        assert(!cache->pixels && read_gpu(canvas,67,71)==0xffffff);
        assert(read_gpu(canvas,3,7)==0); /* Fully transparent source leaves target alone. */
        surface_destroy(cache);cache=NULL;
        gui_gpu_icon(canvas,&cache,icon,2,2,false,3,7,65,true,0x88cc44);
        assert(gui_gpu_flush() && read_gpu(canvas,67,71)==0x88cc44);
        surface_destroy(cache);cache=NULL;
        uint8_t mask[1]={255};gui_gpu_icon(canvas,&cache,mask,1,1,true,0,0,1,true,0x432187);
        assert(gui_gpu_flush() && read_gpu(canvas,0,0)==0x432187);
        surface_destroy(cache);surface_destroy(canvas);
        canvas=surface_create(80,80);assert(gui_gpu_attach(canvas));
        surface_t *assets[12]={0};
        for(unsigned i=0;i<12;i++)gui_gpu_icon(canvas,&assets[i],icon,2,2,false,0,0,32,false,0);
        assert(gui_gpu_flush());
        unsigned resident=0;for(unsigned i=0;i<12;i++)resident+=assets[i]!=NULL;
        assert(resident==8 && !assets[0] && !assets[3] && assets[11]);
        for(unsigned i=0;i<12;i++)surface_destroy(assets[i]);
        surface_destroy(canvas);
    }
    surface_t s=make_surface(128,96),other=make_surface(19,13);
    {
        surface_t *native=surface_create(320,220),*cpu=surface_create(320,220);
        assert(gui_gpu_attach(native));
        for(int spread=1;spread<=56;spread+=5){
            gui_clear(native,0x314253);gui_clear(cpu,0x314253);assert(gui_gpu_flush());
            unsigned first=draws;
            gui_soft_shadow(native,rect_make(51,42,172,109),spread,90,12);
            gui_soft_shadow(cpu,rect_make(51,42,172,109),spread,90,12);
            check(native,cpu->pixels);assert(draws==first+1);
        }
        puts("PASS actual scaled window shadow: exact CPU-path pixels, one GPU command/batch, no CPU target");
        gui_clear(native,0x314253);assert(gui_gpu_flush());unsigned first=draws;
        legacy_gui_soft_shadow(native,rect_make(51,42,172,109),28,90,12);
        assert(gui_gpu_flush());unsigned former=draws-first;
        gui_clear(native,0x314253);assert(gui_gpu_flush());first=draws;
        gui_soft_shadow(native,rect_make(51,42,172,109),28,90,12);
        assert(gui_gpu_flush() && draws==first+1 && former>20);
        printf("PASS actual scale-2 shadow: %u former pixel-command batches -> 1 shader command/batch\n",former);
        surface_destroy(native);surface_destroy(cpu);
    }
    {
        surface_t *native=surface_create(80,80),*cpu=surface_create(80,80);assert(gui_gpu_attach(native));
        uint32_t icon[6]={0x00347891,0xffabcdef,0x881245ac,0x77118822,0x12123456,0xff238954};
        uint8_t alpha[6]={0,255,128,64,129,1};
        gui_register_app_icon(0,icon,3,2);gui_register_icon_mask(0,alpha,3,2);
        unsigned first=draws;
        legacy_app_icon_blit(native,0,0,0,80,false,0);assert(gui_gpu_flush());
        unsigned legacy_batches=draws-first;
        first=draws;app_icon_blit(native,0,0,0,80,false,0);assert(gui_gpu_flush());
        unsigned resident_batches=draws-first;
        assert(legacy_batches>=20 && resident_batches==1);
        printf("PASS actual 80x80 icon: %u former per-pixel batches -> %u resident-image batch\n",legacy_batches,resident_batches);
        gui_clear(native,0);assert(gui_gpu_flush());
        for(int tint=0;tint<2;tint++)for(int size=1;size<=73;size+=12) {
            app_icon_blit(cpu,0,-3,4,size,tint,0xc1785a);app_icon_blit(native,0,-3,4,size,tint,0xc1785a);
            draw_icon_mask(cpu,&icon_masks[0],9,-2,size,0x721aac);
            draw_icon_mask(native,&icon_masks[0],9,-2,size,0x721aac);
            check(native,cpu->pixels);
        }
        /* Replacing a registered bitmap retires and invalidates its GPU source. */
        app_icon_blit(native,0,0,0,32,false,0);
        gui_register_app_icon(0,icon,3,2);assert(!app_icons[0].gpu_image);
        surface_destroy(icon_masks[0].gpu_image);
        free(app_icons[0].argb);free(icon_masks[0].a);
        surface_destroy(native);surface_destroy(cpu);
    }
    uint32_t expected[128*96]={0};
    busy_count=3;
    gui_gpu_paint(&s,rect_make(0,0,128,96),KG2D_SOLID,0x123456,0,255);
    for(unsigned i=0;i<128*96;i++)expected[i]=0x123456;
    clipped=&s;test_clip=rect_make(7,9,82,61);
    gui_gpu_paint(&s,rect_make(-9,-5,151,100),KG2D_GRADIENT_H,0x102030,0xfedcba,255);
    for(int y=9;y<70;y++)for(int x=7;x<89;x++)expected[y*128+x]=mix(0x102030,0xfedcba,(x+9)*255/150);
    gui_gpu_paint(&other,rect_make(0,0,19,13),KG2D_SOLID,0xffcc88,0,255);
    clipped=NULL;
    for(int i=0;i<800;i++){
        int x=i*17%128,y=i*7%96;
        gui_gpu_paint(&s,rect_make(x,y,1,1),KG2D_SOLID,0xf0318a,0,113);
        expected[y*128+x]=mix(expected[y*128+x],0xf0318a,113);
    }
    check(&s,expected);assert(sleeps==3);
    surface_t widgets=make_surface(128,96);
    uint32_t widget_reference[128*96]={0};
    surface_t software={.pixels=widget_reference,.width=128,.height=96,.stride=128};
    paint_widgets(&software);paint_widgets(&widgets);check(&widgets,widget_reference);
    gui_gpu_detach(&widgets);
    {
        /* A resident source needs no leading submission when preceding
         * destination paints can share its ordered command batch. Keep the
         * trailing completion: callers may mutate/free the source on return. */
        surface_t target=make_surface(32,24),source=make_surface(8,6);
        surface_t alternate=make_surface(8,6);
        gui_clear(&source,0x88cc44);gui_clear(&alternate,0x2244bb);
        assert(gui_gpu_flush());
        uint32_t pixels[32*24];
        for(int alpha=133;alpha<=255;alpha+=122) {
            unsigned first=draws;
            gui_clear(&target,0x123456);
            gui_gpu_blit(&target,&source,rect_make(0,0,8,6),rect_make(3,4,20,15),alpha);
            assert(draws==first+1 && !batch.count);
            for(int y=0;y<24;y++)for(int x=0;x<32;x++)
                pixels[y*32+x]=(x>=3&&x<23&&y>=4&&y<19)?mix(0x123456,0x88cc44,alpha):0x123456;
            check(&target,pixels);
        }
        /* An unsubmitted write TO the source must complete before sampling. */
        unsigned first=draws;
        gui_clear(&source,0xabcdef);
        gui_gpu_blit(&target,&source,rect_make(0,0,8,6),rect_make(0,0,32,24),255);
        assert(draws==first+2&&!batch.count);
        for(unsigned i=0;i<32*24;i++)pixels[i]=0xabcdef;
        check(&target,pixels);
        /* Different source bindings still split. Do not change the earlier
         * image's meaning by replacing a batch's source before retirement. */
        kg2d_command_t prior={.x=0,.y=0,.width=16,.height=24,
            .op=KG2D_IMAGE_SCALED,.opacity=255,.source_stride=source.gpu->pitch,
            .reserved={8,6,0,0}};
        first=draws;assert(enqueue(&target,prior,source.gpu->handle));
        gui_gpu_blit(&target,&alternate,rect_make(0,0,8,6),rect_make(16,0,16,24),255);
        assert(draws==first+2&&!batch.count);
        for(int y=0;y<24;y++)for(int x=0;x<32;x++)pixels[y*32+x]=x<16?0xabcdef:0x2244bb;
        check(&target,pixels);
        gui_gpu_detach(&target);gui_gpu_detach(&source);gui_gpu_detach(&alternate);
        puts("PASS resident composition: paint+blit two submissions -> one; exact alpha/order, source-write and source-switch barriers retained");
    }
    /* Consecutive glyph masks must keep distinct packed source offsets. */
    unsigned before=draws;
    for(int i=0;i<80;i++){
        unsigned char mask[35];for(int j=0;j<35;j++)mask[j]=(j*19+i*7)&255;
        int x=i%16*8,y=i/16*11;
        gui_gpu_mask(&s,mask,5,7,5,x,y,0x99cc11,181);
        for(int v=0;v<7;v++)for(int u=0;u<5;u++)expected[(y+v)*128+x+u]=mix(expected[(y+v)*128+x+u],0x99cc11,mask[v*5+u]*181/255);
    }
    check(&s,expected);assert(draws-before<80); /* batched, not launch per glyph */
    /* Resident scaled composition, source changes, and cropped view stride. */
    surface_t view=other;view.width=11;view.height=9;
    gui_gpu_blit(&s,&view,rect_make(2,1,9,8),rect_make(12,17,61,39),133);
    for(int y=17;y<56;y++)for(int x=12;x<73;x++)expected[y*128+x]=mix(expected[y*128+x],0xffcc88,133);
    check(&s,expected);
    uint32_t snapshot[128*96];memcpy(snapshot,expected,sizeof snapshot);
    gui_gpu_blit(&s,&s,rect_make(10,10,51,39),rect_make(19,17,51,39),255);
    for(int y=0;y<39;y++)for(int x=0;x<51;x++)expected[(y+17)*128+x+19]=snapshot[(y+10)*128+x+10];
    check(&s,expected);
    /* Explicit CPU asset upload; not a CPU desktop frame. */
    uint32_t asset[15];for(int i=0;i<15;i++)asset[i]=i*0x10203;
    surface_t image={.pixels=asset,.width=5,.height=3,.stride=5};
    gui_gpu_blit(&s,&image,rect_make(0,0,5,3),rect_make(20,20,25,18),255);
    for(int y=20;y<38;y++)for(int x=20;x<45;x++)expected[y*128+x]=asset[(y-20)*3/18*5+(x-20)*5/25];
    check(&s,expected);
    {
        // Partially clipped scaling retains the ORIGINAL source/destination
        // mapping. Test a non-uniform pattern, not a constant-colour image.
        uint32_t pattern[15];for(unsigned i=0;i<15;i++)pattern[i]=0x031527u*i;
        surface_t cpu_pattern={.pixels=pattern,.width=5,.height=3,.stride=5};
        surface_t *gpu_pattern=gui_gpu_image(pattern,5,3);assert(gpu_pattern);
        for(unsigned route=0;route<2;route++) {
            rect_t to=rect_make(-3,2,25,18),clip=rect_make(1,5,12,9);
            clipped=&s;test_clip=clip;
            gui_gpu_blit(&s,route?gpu_pattern:&cpu_pattern,rect_make(0,0,5,3),to,117);
            for(int y=5;y<14;y++)for(int x=1;x<13;x++)
                expected[y*128+x]=mix(expected[y*128+x],pattern[(y-2)*3/18*5+(x+3)*5/25],117);
            check(&s,expected);
        }
        uint32_t cp[32*32];for(unsigned i=0;i<32*32;i++)cp[i]=0x112233;
        surface_t capture={.pixels=cp,.width=32,.height=32,.stride=32};
        clipped=&capture;test_clip=rect_make(1,5,12,9);
        gui_gpu_blit(&capture,gpu_pattern,rect_make(0,0,5,3),rect_make(-3,2,25,18),117);
        for(int y=0;y<32;y++)for(int x=0;x<32;x++) {
            uint32_t want=0x112233;
            if(x>=1&&x<13&&y>=5&&y<14)
                want=mix(want,pattern[(y-2)*3/18*5+(x+3)*5/25],117);
            assert(cp[y*32+x]==want);
        }
        clipped=NULL;surface_destroy(gpu_pattern);
        puts("PASS partial scaled clipping: non-uniform CPU/GPU sources and GPU-to-CPU capture preserve original sample coordinates and outside pixels");
    }
    assert(gui_gpu_present(&s,rect_make(0,0,128,96))&&presents==1);
    uint32_t capture_pixels[128*96]={0};
    surface_t capture={.pixels=capture_pixels,.width=128,.height=96,.stride=128};
    gui_gpu_blit(&capture,&s,rect_make(0,0,128,96),rect_make(0,0,128,96),255);
    assert(!memcmp(capture_pixels,expected,sizeof expected) && s.gpu && !s.pixels);
    assert(gui_gpu_cpu_access(&other));assert(!other.gpu);
    for(int i=0;i<19*13;i++)assert(other.pixels[i]==0xffcc88);
    /* Target-owned depth allocation, mixed 2D-before-3D ordering, explicit
     * readback and destruction of BOTH allocations. */
    surface_t target3d=make_surface(71,49);
    kg3d_command_t clear3d={.flags=KG3D_CLEAR_DEPTH,.width=71,.height=49};
    clear3d.v[0].z=0.375f;
    gui_gpu_paint(&target3d,rect_make(0,0,71,49),KG2D_SOLID,0x123456,0,255);
    unsigned prior_draws=draws,prior_creates=creates;
    assert(gui_gpu_draw3d(&target3d,NULL,&clear3d,1,rect_make(0,0,71,49)));
    assert(draws==prior_draws+1 && creates==prior_creates+1 && !target3d.pixels);
    uint64_t zh=target3d.gpu->depth_handle,ch=target3d.gpu->handle;
    colour_t colour_values[49*75];
    for(unsigned i=0;i<49*75;i++)colour_values[i]=0xdeadbeef;
    assert(!gui_gpu_colour_readback(NULL,colour_values,75,49*75));
    assert(!gui_gpu_colour_readback(&target3d,NULL,75,49*75));
    assert(!gui_gpu_colour_readback(&target3d,colour_values,70,49*75));
    assert(!gui_gpu_colour_readback(&target3d,colour_values,75,48*75+70));
    surface_t colour_view=target3d;colour_view.owns_pixels=false;
    assert(!gui_gpu_colour_readback(&colour_view,colour_values,75,49*75));
    assert(gui_gpu_colour_readback(&target3d,colour_values,75,48*75+71));
    assert(target3d.gpu->handle==ch && !target3d.pixels);
    for(unsigned y=0;y<49;y++)for(unsigned x=0;x<75;x++)
        assert(colour_values[y*75+x]==(x<71?0x123456:0xdeadbeef));
    float depth_values[49*75];for(unsigned i=0;i<49*75;i++)depth_values[i]=-99;
    assert(gui_gpu_depth_readback(&target3d,depth_values,75));
    for(unsigned y=0;y<49;y++)for(unsigned x=0;x<75;x++)
        assert(depth_values[y*75+x]==(x<71?0.375f:-99));
    assert(gui_gpu_draw3d(&target3d,NULL,&clear3d,1,rect_make(0,0,71,49)));
    assert(target3d.gpu->depth_handle==zh && creates==prior_creates+1);
    gui_gpu_detach(&target3d);assert(!memories[zh].data && !memories[ch].data);
    /* Preserve resolution with two row strips when a 64-command batch exceeds
     * the kernel work limit. Depth belongs to this larger target independently. */
    surface_t large=make_surface(2048,1024);
    kg3d_command_t many[64];for(unsigned i=0;i<64;i++){many[i]=clear3d;many[i].width=2048;many[i].height=1024;}
    unsigned prior3d=draws3d;
    assert(gui_gpu_draw3d(&large,NULL,many,64,rect_make(0,0,2048,1024)));
    assert(draws3d==prior3d+2);zh=large.gpu->depth_handle;ch=large.gpu->handle;
    prior3d=draws3d;
    for(unsigned i=0;i<64;i++){
        many[i].flags=KG3D_LINE|KG3D_DEPTH_TEST;
        for(unsigned j=0;j<3;j++)many[i].v[j].inv_w=1;
    }
    assert(gui_gpu_draw3d(&large,NULL,many,64,rect_make(0,0,2048,1024)));
    assert(draws3d==prior3d+13 && large.gpu->depth_handle==zh && !large.pixels);
    gui_gpu_detach(&large);assert(!memories[zh].data && !memories[ch].data);
    /* Immutable texture uploads span multiple 4MiB transfers and preserve
     * padded pitch without retaining a CPU shadow on the GPU source object. */
    colour_t *big_image=malloc(1001u*1100u*4u);
    for(unsigned i=0;i<1001u*1100u;i++)big_image[i]=i^0xa53189ffu;
    unsigned prior_uploads=uploads;
    surface_t *resident=gui_gpu_image(big_image,1001,1100);assert(resident && !resident->pixels);
    assert(uploads==prior_uploads+2);
    memory_t *im=&memories[resident->gpu->handle];
    for(unsigned y=0;y<1100;y++)assert(!memcmp(im->data+(size_t)y*im->pitch,big_image+y*1001u,1001u*4u));
    surface_destroy(resident);free(big_image);
    /* A rejected application canvas cannot be sampled, captured or presented,
     * but must not poison independent desktop canvases. Depth handle is shared
     * with the existing fixed raster path and created only once. */
    surface_t broken=make_surface(31,23);gui_gpu_target_t target;
    unsigned c_before=creates;
    gui_gpu_paint(&broken,rect_make(0,0,31,23),KG2D_SOLID,0xabcdef,0,255);
    assert(gui_gpu_target(&broken,true,&target)&&creates==c_before+1);
    assert(target.colour==broken.gpu->handle&&target.depth==broken.gpu->depth_handle);
    assert(gui_gpu_target(&broken,true,&target)&&creates==c_before+1);
    assert(gui_gpu_flush());unsigned draws_before=draws;
    gui_gpu_invalidate(&broken);assert(!batch.failed);
    assert(!gui_gpu_colour_readback(&broken,colour_values,75,49*75));
    unsigned scratch_before=scratch_used,uploads_before=uploads;
    /* Repeated rejected glyphs must never consume shared mask scratch or fall
     * through to a nonexistent CPU canvas. They can carry no source pointer. */
    for(unsigned i=0;i<10000;i++)assert(gui_gpu_mask(&broken,NULL,100,100,100,0,0,0,255));
    assert(scratch_used==scratch_before&&uploads==uploads_before&&!batch.count&&!batch.failed);
    gui_fill(&broken,rect_make(0,0,31,23),0x123456);assert(!broken.pixels&&!batch.count);
    assert(!gui_gpu_target(&broken,false,&target)&&!gui_gpu_attach(&broken));
    assert(!gui_gpu_present(&broken,rect_make(0,0,31,23))&&presents==1);
    assert(gui_gpu_blit(&s,&broken,rect_make(0,0,31,23),rect_make(0,0,31,23),255));
    assert(gui_gpu_blit(&capture,&broken,rect_make(0,0,31,23),rect_make(0,0,31,23),255));
    assert(gui_gpu_flush()&&draws==draws_before);
    gui_gpu_paint(&s,rect_make(0,0,1,1),KG2D_SOLID,0x123456,0,255);
    assert(gui_gpu_flush()&&draws==draws_before+1&&!batch.failed);
    gui_gpu_detach(&broken);
    /* Failed dispatch must not fall back to CPU stores or present stale data. */
    fail_draw=true;
    gui_gpu_paint(&s,rect_make(0,0,20,20),KG2D_SOLID,0xff0000,0,255);
    assert(!gui_gpu_present(&s,rect_make(0,0,128,96))&&presents==1);
    scratch_before=scratch_used;uploads_before=uploads;
    // A failed shared batch cannot consume more mask scratch or dereference
    // glyph data while recovery/quarantine is pending.
    for(unsigned i=0;i<1000;i++)assert(gui_gpu_mask(&s,NULL,10,10,10,0,0,0,255));
    assert(scratch_used==scratch_before&&uploads==uploads_before);
    assert(!s.pixels);
    gui_gpu_detach(&s);free(s.pixels);free(other.pixels);
    // Independent mocked-driver fixtures for failure during cache retirement.
    // Resetting this host model is teardown/setup, NOT a native recovery path.
    memset(&batch,0,sizeof batch);scratch_used=0;fail_draw=false;
    for(unsigned failure=0;failure<2;failure++) {
        surface_t *canvas=surface_create_target(32,32,true),*cache=NULL,*spare=NULL;assert(canvas);
        uint32_t pixel=0xff123456;
        gui_gpu_icon(canvas,&cache,&pixel,1,1,false,0,0,4,false,0);assert(cache);
        gui_gpu_icon(canvas,&spare,&pixel,1,1,false,8,8,4,false,0);assert(spare);
        uint64_t handle=cache->gpu->handle;
        unsigned dead=destroys,attempts=create_attempts;
        live_limit=creates-destroys;
        if(failure==0)fail_draw=true;
        else {assert(gui_gpu_flush());fail_destroy=true;}
        assert(!surface_create_target(16,16,true));
        assert(create_attempts==attempts+1&&destroys==dead&&memories[handle].data);
        if(failure==0)assert(cache&&batch.failed);
        else assert(!cache&&!batch.failed); // no success/retry after failed destroy
        live_limit=0;fail_draw=false;fail_destroy=false;
        memset(&batch,0,sizeof batch);scratch_used=0;
        if(cache)surface_destroy(cache);
        else {free(memories[handle].data);memories[handle].data=NULL;destroys++;}
        surface_destroy(spare);
        surface_destroy(canvas);
    }
    puts("PASS reclamation failures: pending draw failure preserves cache; failed destroy prevents CREATE retry and retains modeled GPU storage");
    printf("PASS actual desktop GPU backend: %u draws, %u uploads; clipping, 800 ordered paints, 80 packed masks, resident scale/composition, immutable CPU shadow, busy retry, cleanup and failure retention (modeled syscalls)\n",draws,uploads);
    return 0;
}
'''
    run_test(c,'gui_gpu_backend')

if __name__=='__main__':main()
