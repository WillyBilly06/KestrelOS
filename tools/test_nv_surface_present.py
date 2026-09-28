#!/usr/bin/env python3
"""Actual layout-damage and all-head surface presentation, with modeled outputs."""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]

def main():
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
'''
    source += '#include "' + (ROOT/'include/kestrel/display_layout.h').as_posix() + '"\n'
    layout = (ROOT/'kernel/display_layout.c').read_text()
    source += '\n'.join(l for l in layout.splitlines() if not l.startswith('#include'))
    source += r'''
#define KESTREL_NVKMS_MAX_TEST_DISPLAYS 4
typedef struct {u32 width,height,pitch;u64 bytes;} nv_surface_info_t;
typedef struct {struct {struct {u32 hVisible,vVisible;} timings;} mode; int index;} test_display_t;
static test_display_t active[4];
static u32 active_count;
static bool runtime_ready;
static dl_layout_t runtime_layout;
static unsigned selects,presents;
static int selected,fail_select=-1,fail_present=-1;
static u32 *pixels[4];
static bool nv_surface_info(u64 owner,u64 handle,nv_surface_info_t *info) {
    if(owner!=7 || handle!=123) return false;
    *info=(nv_surface_info_t){(u32)runtime_layout.fb_width,(u32)runtime_layout.fb_height,
                             (u32)runtime_layout.fb_width*4,0};return true;
}
static bool runtime_select(test_display_t *d) {
    selects++;selected=d->index;return selected!=fail_select;
}
static u32 colour(u32 x,u32 y) {return 0xff000000u|((x*7919u+y*1237u)&0xffffffu);}
static void sample_xy(const dl_scanout_t *s,u32 x,u32 y,u32 *sx,u32 *sy) {
    switch(s->rotation){
    case 0: *sx=s->src_x+(u64)x*s->src_w/s->out_w;*sy=s->src_y+(u64)y*s->src_h/s->out_h;break;
    case 90: *sx=s->src_x+(u64)y*s->src_w/s->out_h;*sy=s->src_y+(u64)(s->out_w-1-x)*s->src_h/s->out_w;break;
    case 180: *sx=s->src_x+(u64)(s->out_w-1-x)*s->src_w/s->out_w;*sy=s->src_y+(u64)(s->out_h-1-y)*s->src_h/s->out_h;break;
    case 270: *sx=s->src_x+(u64)(s->out_h-1-y)*s->src_w/s->out_h;*sy=s->src_y+(u64)x*s->src_h/s->out_w;break;
    default: assert(false);
    }
}
static bool nv_surface_present(u64 owner,u64 handle,u32 vx,u32 vy,u32 vw,u32 vh,u32 x,u32 y,u32 w,u32 h,u32 rotation) {
    assert(owner==7 && handle==123);presents++;
    if(selected==fail_present) return false;
    dl_scanout_t *s=&runtime_layout.out[selected];
    assert(vx==(u32)s->src_x && vy==(u32)s->src_y && vw==(u32)s->src_w && vh==(u32)s->src_h);
    assert(rotation==(u32)s->rotation);
    assert((u64)x+w<=(u32)s->out_w && (u64)y+h<=(u32)s->out_h);
    for(u32 oy=y;oy<y+h;oy++) for(u32 ox=x;ox<x+w;ox++) {
        u32 sx,sy;sample_xy(s,ox,oy,&sx,&sy);
        pixels[selected][(u64)oy*s->out_w+ox]=colour(sx,sy);
    }
    return true;
}
'''
    source += function((ROOT/'kernel/nvkms_kapi_client.c').read_text(), 'nvkms_kapi_runtime_present_surface')
    source += r'''
static u32 random_value=123;
static u32 rnd(void) {return random_value=random_value*1664525u+1013904223u;}
static void geometry(void) {
    for(unsigned iteration=0;iteration<100000;iteration++) {
        dl_scanout_t s={1,(int)(rnd()%31),(int)(rnd()%31),1+(int)(rnd()%37),1+(int)(rnd()%37),1+(int)(rnd()%41),1+(int)(rnd()%41)};
        s.rotation=(int)(rnd()%4)*90;
        int x=(int)(rnd()%101)-35,y=(int)(rnd()%101)-35,w=(int)(rnd()%71),h=(int)(rnd()%71);
        int ox=-123,oy=-123,ow=-123,oh=-123;
        int r=display_layout_damage(&s,x,y,w,h,&ox,&oy,&ow,&oh);assert(r>=0);
        bool any=false;
        for(int py=0;py<s.out_h;py++) for(int px=0;px<s.out_w;px++) {
            u32 sx,sy;sample_xy(&s,px,py,&sx,&sy);
            bool touched=(long long)sx>=x && sx<(long long)x+w && (long long)sy>=y && sy<(long long)y+h;
            bool emitted=r==1 && px>=ox && px<ox+ow && py>=oy && py<oy+oh;
            assert(touched==emitted); any|=touched;
        }
        assert((r==1)==any); if(!r) assert(ox==-123 && oy==-123 && ow==-123 && oh==-123);
    }
    dl_scanout_t s={1,INT_MAX,INT_MAX,INT_MAX,INT_MAX,INT_MAX,INT_MAX};
    int x,y,w,h;
    assert(display_layout_damage(&s,INT_MAX,INT_MAX,INT_MAX,INT_MAX,&x,&y,&w,&h)==1);
    assert(x==0 && y==0 && w==INT_MAX && h==INT_MAX);
    assert(display_layout_damage(&s,0,0,-1,1,&x,&y,&w,&h)==-1);
    assert(display_layout_damage(NULL,0,0,1,1,&x,&y,&w,&h)==-1);
    s.rotation=45;
    assert(display_layout_damage(&s,0,0,1,1,&x,&y,&w,&h)==-1);
}
static void verify(int dx,int dy,int dw,int dh) {
    for(u32 i=0;i<active_count;i++) {
        dl_scanout_t *s=&runtime_layout.out[i];
        for(u32 y=0;y<active[i].mode.timings.vVisible;y++)
            for(u32 x=0;x<active[i].mode.timings.hVisible;x++) {
                u32 expected=0xa55a1234;
                if(s->active) {
                    u32 sx,sy;sample_xy(s,x,y,&sx,&sy);
                    if(sx>=(u32)dx && sx<(u32)(dx+dw) && sy>=(u32)dy && sy<(u32)(dy+dh)) expected=colour(sx,sy);
                }
                assert(pixels[i][(u64)y*active[i].mode.timings.hVisible+x]==expected);
            }
    }
}
int main(void) {
    geometry();
    const dl_mode_t modes[]={DL_EXTEND,DL_MIRROR,DL_ONLY_PRIMARY,DL_ONLY_SECONDARY,DL_ONLY_OUTPUT(2),DL_ONLY_OUTPUT(3)};
    dl_output_size_t sizes[4]={{101,89},{53,37},{173,113},{83,151}};
    runtime_ready=true;
    for(active_count=1;active_count<=4;active_count++) for(unsigned m=0;m<sizeof modes/sizeof modes[0];m++) {
        display_layout_compute(sizes,active_count,modes[m],&runtime_layout);
        for(u32 i=0;i<active_count;i++) {
            active[i].mode.timings.hVisible=sizes[i].width;active[i].mode.timings.vVisible=sizes[i].height;active[i].index=(int)i;
            pixels[i]=malloc((size_t)sizes[i].width*sizes[i].height*4);assert(pixels[i]);
        }
        for(unsigned rotation=0;rotation<4;rotation++) for(unsigned partial=0;partial<2;partial++) {
            for(u32 i=0;i<active_count;i++) runtime_layout.out[i].rotation=((rotation+i)%4)*90;
            for(u32 i=0;i<active_count;i++) for(int p=0;p<sizes[i].width*sizes[i].height;p++) pixels[i][p]=0xa55a1234;
            int x=partial?7:0,y=partial?11:0,w=runtime_layout.fb_width-x-(partial?3:0),h=runtime_layout.fb_height-y-(partial?5:0);
            selects=presents=0;
            assert(nvkms_kapi_runtime_present_surface(7,123,x,y,w,h));verify(x,y,w,h);
            assert(!nvkms_kapi_runtime_present_surface(8,123,x,y,w,h));
        }
        for(u32 i=0;i<active_count;i++) {free(pixels[i]);pixels[i]=NULL;}
    }
    active_count=3;display_layout_compute(sizes,3,DL_MIRROR,&runtime_layout);
    for(u32 i=0;i<3;i++) {active[i].mode.timings.hVisible=sizes[i].width;active[i].mode.timings.vVisible=sizes[i].height;
        active[i].index=i;pixels[i]=calloc((size_t)sizes[i].width*sizes[i].height,4);}
    for(int i=0;i<3;i++) {
        fail_select=i; selects=presents=0;
        assert(!nvkms_kapi_runtime_present_surface(7,123,0,0,101,89));assert(selects==(unsigned)i+1 && presents==(unsigned)i);
        fail_select=-1;fail_present=i;selects=presents=0;
        assert(!nvkms_kapi_runtime_present_surface(7,123,0,0,101,89));assert(presents==(unsigned)i+1);fail_present=-1;
    }
    selects=presents=0;runtime_layout.out[2].src_w=INT_MAX;
    assert(!nvkms_kapi_runtime_present_surface(7,123,0,0,101,89));assert(!selects && !presents);
    for(unsigned i=0;i<3;i++)free(pixels[i]);
    puts("PASS: 100000 exact scaled/rotated damage mappings, all four rotations, 1-4 outputs across six layouts, whole/partial presentation, preflight and per-head failures (modeled outputs)");
}
'''
    run_test(source, 'nv_surface_present')

if __name__ == '__main__':
    main()
