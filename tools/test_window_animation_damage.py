#!/usr/bin/env python3
"""Execute animation advancement/painting with modeled surfaces and time."""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / 'user/libgui/window.c').read_text()
    draw = (ROOT / 'user/libgui/draw.c').read_text()
    header = (ROOT / 'user/libgui/window.h').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {int x,y,w,h;} rect_t;
typedef uint32_t colour_t;
typedef struct {int width,height;void *gpu;rect_t clip;int stride;colour_t *pixels;} surface_t;
typedef struct {int window;} theme_t;
static theme_t g_theme;
#define ANIM_ONE 1000
#define ANIM_DEFAULT_MS 180
#define WIN_MINIMISED 1
#define WM_MAX_DAMAGE 12
#define WIN_RADIUS (7*gui_scale())
'''
    c += re.search(r'typedef enum \{\s*ANIM_NONE[\s\S]*?\} anim_kind;', header)[0]
    c += re.search(r'typedef struct \{\s*anim_kind kind;[\s\S]*?\} window_anim;', header)[0]
    c += r'''
typedef struct window {rect_t frame;window_anim anim;struct window *above;
    bool needs_paint,closed;int state;} window_t;
typedef struct {window_t *bottom;bool animations_on,animating,full_redraw,running;
    rect_t damage[WM_MAX_DAMAGE];int damage_count;} wm_t;
static int scale=1,allocations,destroys,blits,fallbacks,fail_alloc,compositions;
static int max_alloc_pixels;
static uint64_t cleared_pixels;
static unsigned steps;
static uint64_t clock_ms;
static rect_t painted;
static int painted_alpha;
static surface_t *stage;
static uint64_t uptime_ms(void){return clock_ms;}
static int gui_scale(void){return scale;}
static int gui_screen_scale(void){return scale;}
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static bool same(rect_t a,rect_t b){return a.x==b.x&&a.y==b.y&&a.w==b.w&&a.h==b.h;}
static rect_t rect_union(rect_t a,rect_t b){int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
return rect_make(x,y,r-x,d-y);}
static void wm_damage_all(wm_t *wm){wm->full_redraw=true;}
static void wm_close(wm_t *wm,window_t *w){(void)wm;w->closed=true;}
static surface_t *surface_create_target(int w,int h,bool gpu){
    if(fail_alloc||(max_alloc_pixels&&w*h>max_alloc_pixels))return NULL;
    surface_t *s=calloc(1,sizeof *s);assert(s);
    *s=(surface_t){.width=w,.height=h,.gpu=gpu?(void*)1:NULL,.stride=w};
    s->pixels=calloc((size_t)w*h,sizeof *s->pixels);assert(s->pixels);allocations++;return s;
}
static void surface_destroy(surface_t *s){assert(s);destroys++;free(s->pixels);free(s);}
static void surface_reset_clip(surface_t *s){s->clip=rect_make(0,0,s->width,s->height);}
static void surface_set_clip(surface_t *s,rect_t r){s->clip=r;}
static rect_t surface_clip(surface_t *s){return s->clip;}
#define KG2D_SOLID 0
static rect_t rect_intersection(rect_t a,rect_t b){
    int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r>x?r-x:0,d>y?d-y:0);
}
static bool rect_empty(rect_t r){return r.w<=0||r.h<=0;}
static void text_runs_covered(const surface_t *s,rect_t r){(void)s;(void)r;}
static void blit_fill_row(colour_t *p,int n,colour_t c){
    for(int i=0;i<n;i++)p[i]=c;cleared_pixels+=n;
}
static bool gui_gpu_paint(surface_t *s,rect_t r,int op,colour_t c,colour_t unused,int alpha){
    assert(op==KG2D_SOLID&&unused==0&&alpha==255);
    if(!s->gpu)return false;
    for(int y=r.y;y<r.y+r.h;y++)blit_fill_row(s->pixels+(size_t)y*s->stride+r.x,r.w,c);
    return true;
}
static void paint_frame_at(wm_t *wm,surface_t *s,window_t *w,rect_t r,bool shadow,bool hover){
    (void)wm;(void)s;(void)w;(void)r;assert(!shadow&&!hover);compositions++;
}
static void paint_frame(wm_t *wm,surface_t *s,window_t *w){(void)wm;(void)s;fallbacks++;painted=w->frame;}
static void gui_soft_shadow(surface_t *s,rect_t r,int spread,int alpha,int radius){
    (void)s;(void)r;(void)spread;(void)alpha;(void)radius;
}
static void gui_blit_scaled(surface_t *s,surface_t *view,rect_t r,int alpha){
    assert((s->gpu!=NULL)==(view->gpu!=NULL));painted=r;painted_alpha=alpha;blits++;
}
'''
    for name in ['rect_intersects', 'anim_lerp', 'anim_lerp_rect', 'anim_ease_in', 'anim_ease_out', 'anim_ease_in_out']:
        c += '\n' + function(draw, name)
    for name in ['gui_fill', 'gui_clear']:
        c += '\n' + function(draw, name)
    for name in ['merge_waste', 'wm_damage', 'window_visual_bounds', 'window_reaches_area', 'wm_animate',
                 'anim_progress', 'step_animations', 'wm_is_animating']:
        c += '\n' + function(src, name)
    start = src.index('static surface_t *stage_for(')
    c += '\n' + src[start:src.index('\n}', start) + 2]
    c += '\n' + function(src, 'stage_retire_if_idle')
    c += '\n' + function(src, 'paint_animating')
    c += r'''
static void covered(wm_t *wm,rect_t frame){
    // Conservative independent bound contains the animated 10px shadow and
    // the unscaled fallback's largest 14*scale shadow, including its offset.
    rect_t r={frame.x-14*scale,frame.y-14*scale,frame.w+28*scale,frame.h+28*scale+4};
    bool yes=wm->full_redraw;
    for(int i=0;i<wm->damage_count;i++){
        rect_t d=wm->damage[i];
        yes|=d.x<=r.x&&d.y<=r.y&&d.x+d.w>=r.x+r.w&&d.y+d.h>=r.y+r.h;
    }
    assert(yes);
}
int main(void){
    surface_t screen={.width=7680,.height=1440,.gpu=(void*)1,.clip={0,0,7680,1440}};
    for(scale=1;scale<=4;scale++)for(int kind=ANIM_OPEN;kind<=ANIM_GEOMETRY;kind++) {
        rect_t from={200,180,600,400},to={340,250,400,260};
        window_t w={.frame=to};wm_t wm={.bottom=&w,.animations_on=true};
        clock_ms=1000;wm_animate(&wm,&w,kind,from,to,255,127,160);
        assert(w.anim.paint_progress==0&&same(w.anim.damage_frame,from));
        rect_t old=from;
        for(int elapsed=0;elapsed<160;elapsed+=16) {
            wm.full_redraw=false;wm.damage_count=0;clock_ms=1000+elapsed;
            assert(step_animations(&wm));assert(!wm.full_redraw);
            covered(&wm,old);covered(&wm,w.anim.damage_frame);covered(&wm,w.frame);
            old=w.anim.damage_frame;
            int sample=w.anim.paint_progress,alpha=anim_lerp(255,127,sample);
            int n=blits;
            // Slow painting across several rectangles must not advance the
            // animation beyond the position whose damage was recorded.
            clock_ms+=500;
            for(int region=0;region<3;region++) {
                paint_animating(&wm,&screen,&w);
                assert(same(painted,old)&&painted_alpha==alpha);
            }
            assert(blits==n+3);steps++;
        }
        clock_ms=1160;wm.full_redraw=false;wm.damage_count=0;
        assert(!step_animations(&wm)&&w.anim.kind==ANIM_NONE&&wm.full_redraw);
        if(kind==ANIM_CLOSE)assert(w.closed);
        else if(kind==ANIM_MINIMISE)assert(w.state==WIN_MINIMISED);
        else assert(same(w.frame,to)&&w.needs_paint);
    }
    // Two independent animations on distant monitors: neither may drop the
    // other's footprint or force all displays to repaint while both run.
    window_t left={.frame={200,200,500,300}},right={.frame={5500,220,500,300}};
    left.above=&right;wm_t pair={.bottom=&left,.animations_on=true};
    clock_ms=1800;
    wm_animate(&pair,&left,ANIM_CLOSE,left.frame,(rect_t){220,220,440,260},255,127,160);
    wm_animate(&pair,&right,ANIM_RESTORE,(rect_t){5600,300,200,100},right.frame,255,255,320);
    pair.full_redraw=false;pair.damage_count=0;clock_ms=1840;
    assert(step_animations(&pair)&&!pair.full_redraw);
    covered(&pair,left.anim.from);covered(&pair,left.anim.damage_frame);
    covered(&pair,right.anim.from);covered(&pair,right.anim.damage_frame);
    pair.full_redraw=false;pair.damage_count=0;clock_ms=1960;
    assert(step_animations(&pair)&&left.closed&&right.anim.kind==ANIM_RESTORE);
    // Stage reuse, growth failure, recovery, and renderer changes.
    rect_t f={0,0,100,100};int n=allocations;
    assert(stage_for(f,true)==stage&&allocations==n);
    assert(stage_for(f,false)&&!stage->gpu&&allocations==n+1);
    assert(stage_for(f,true)&&stage->gpu&&allocations==n+2);
    fail_alloc=1;f.w=2000;assert(!stage_for(f,true)&&!stage);
    window_t w={.frame=f};wm_t wm={.bottom=&w,.animations_on=true};
    clock_ms=2000;wm_animate(&wm,&w,ANIM_OPEN,(rect_t){100,100,100,100},f,255,255,160);
    wm.full_redraw=false;wm.damage_count=0;assert(step_animations(&wm));covered(&wm,f);
    n=fallbacks;paint_animating(&wm,&screen,&w);assert(fallbacks==n+1&&same(painted,f));
    fail_alloc=0;paint_animating(&wm,&screen,&w);assert(stage&&stage->gpu);
    // Do not retire while either animation remains, including a new animation
    // started by a tick after step_animations reported idle. Do not trust the
    // cached wm.animating flag, which need not describe that new animation yet.
    wm.running=true;wm.animating=false;n=destroys;
    surface_t *held=stage;
    stage_retire_if_idle(&wm);assert(stage==held&&destroys==n);
    w.anim.kind=ANIM_NONE;
    window_t second={.anim={.kind=ANIM_OPEN}};w.above=&second;
    stage_retire_if_idle(&wm);assert(stage==held&&destroys==n);
    second.anim.kind=ANIM_NONE;wm.animating=true;
    stage_retire_if_idle(&wm);assert(!stage&&destroys==n+1);
    stage_retire_if_idle(&wm);assert(!stage&&destroys==n+1);
    // A future animation allocates again, and stopping the manager releases
    // its staging even when an animation is still marked active.
    for(int gpu=0;gpu<=1;gpu++) {
        assert(stage_for(f,gpu));w.anim.kind=ANIM_OPEN;wm.running=false;n=destroys;
        stage_retire_if_idle(&wm);assert(!stage&&destroys==n+1);
    }
    // Wide/tall windows share one high-water buffer, without reallocating on
    // every pass. Renderer changes need not preserve the old renderer's peak.
    assert(stage_for((rect_t){0,0,512,64},true));n=allocations;
    assert(stage_for((rect_t){0,0,64,512},true));
    assert(stage->width==512&&stage->height==512&&allocations==n+1);
    held=stage;n=allocations;
    for(int i=0;i<100;i++) {
        assert(stage_for((rect_t){0,0,512,64},true)==held);
        assert(stage_for((rect_t){0,0,64,512},true)==held);
    }
    assert(allocations==n);
    assert(stage_for((rect_t){0,0,64,96},false));
    assert(stage->width==64&&stage->height==96&&!stage->gpu);
    stage_retire_if_idle(&wm);assert(!stage);
    // A combined high-water allocation may exceed available VRAM even when
    // the current window fits. Retry only the requested extent in that case.
    max_alloc_pixels=512*128;
    assert(stage_for((rect_t){0,0,512,64},true));
    assert(stage_for((rect_t){0,0,64,512},true));
    assert(stage->width==64&&stage->height==512);
    stage_retire_if_idle(&wm);assert(!stage);
    assert(allocations==destroys);
    // Retained high-water storage must not turn small compositions into full
    // allocation clears. Execute the real gui_clear/gui_fill clipping on both
    // renderer routes and inspect every unused pixel for accidental writes.
    max_alloc_pixels=0;scale=1;
    for(int gpu=0;gpu<=1;gpu++) {
        surface_t destination={.width=7680,.height=1440,.gpu=gpu?(void*)1:NULL,
                               .clip={0,0,7680,1440}};
        assert(stage_for((rect_t){0,0,512,512},gpu));held=stage;n=allocations;
        const rect_t frames[]={{100,100,64,128},{100,100,128,64},{100,100,8,12},{100,100,512,512}};
        for(unsigned fidx=0;fidx<sizeof frames/sizeof *frames;fidx++) {
            for(int i=0;i<512*512;i++)stage->pixels[i]=0xa55a1234;
            w.frame=frames[fidx];w.anim.from=w.anim.to=w.frame;
            w.anim.paint_progress=0;w.anim.alpha_from=w.anim.alpha_to=255;
            uint64_t before=cleared_pixels;paint_animating(&wm,&destination,&w);
            assert(stage==held&&allocations==n);
            assert(cleared_pixels-before==(uint64_t)w.frame.w*w.frame.h);
            for(int y=0;y<512;y++)for(int x=0;x<512;x++)
                assert(stage->pixels[y*512+x]==(x<w.frame.w&&y<w.frame.h?
                                               (colour_t)g_theme.window:0xa55a1234u));
            assert(same(surface_clip(stage),(rect_t){0,0,512,512}));
        }
        stage_retire_if_idle(&wm);assert(!stage);
    }
    assert(allocations==destroys);
    // Distant damage must not compose an animation from another monitor, but
    // retain both animated-shadow and unscaled-fallback footprints. The latter
    // is drawn only when staging allocation fails, so keep it conservative.
    max_alloc_pixels=0;scale=2;
    w.frame=(rect_t){5300,200,512,400};w.anim.from=(rect_t){100,200,256,200};
    w.anim.to=w.anim.from;w.anim.paint_progress=0;
    w.anim.alpha_from=w.anim.alpha_to=255;
    for(int failure=0;failure<=1;failure++) {
        fail_alloc=failure;
        screen.clip=(rect_t){2800,200,500,500};
        int a=allocations,b=blits,c=compositions,d=fallbacks;
        for(int region=0;region<100;region++)paint_animating(&wm,&screen,&w);
        assert(!stage&&allocations==a&&blits==b&&compositions==c&&fallbacks==d);
        // The left shadow extends outside the animated body.
        screen.clip=(rect_t){95,200,5,20};
        paint_animating(&wm,&screen,&w);
        assert(failure?fallbacks==d+1:compositions==c+1);
        stage_retire_if_idle(&wm);
        // Damage at the normal frame still permits the low-memory fallback.
        screen.clip=(rect_t){5400,250,20,20};
        d=fallbacks;c=compositions;paint_animating(&wm,&screen,&w);
        assert(failure?fallbacks==d+1:compositions==c+1);
        stage_retire_if_idle(&wm);
    }
    assert(!stage&&allocations==destroys);
    printf("PASS actual animation: %u intermediate frames, stable position across 600 delayed region paints, old/new/fallback shadow damage, five end states, stage mode/growth/failure/recovery, overlapping/late animation retention, idle/shutdown release, 200 wide/tall reuse calls, combined-allocation failure fallback, bounded CPU/GPU clears with 2097152 active/padding pixels checked, 200 distant damage calls culled, shadow/fallback visibility retained and balanced cleanup (modeled surfaces)\n",steps);
}
'''
    pump = function(src, 'wm_pump')
    moving = pump[pump.index('if (moving) {'):pump.index('} else if (wm->animating)')]
    assert 'wm_damage_all' not in moving
    assert 'if (!wm->running) { stage_retire_if_idle(wm); return true; }' in pump
    assert pump.rindex('stage_retire_if_idle(wm)') > pump.index('repaint(wm)')
    assert 'stage_retire_if_idle(wm);' in function(src, 'wm_run')
    run_test(c, 'window_animation_damage')


if __name__ == '__main__':
    main()
