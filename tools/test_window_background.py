#!/usr/bin/env python3
"""Actual background occlusion code against independent per-pixel coverage."""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / 'user/libgui/window.c').read_text()
    draw = (ROOT / 'user/libgui/draw.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,w,h;} rect_t;
typedef struct {int width,height;rect_t clip;} surface_t;
typedef struct window {rect_t frame;bool visible;int state;
    struct {int kind;} anim;struct window *below;} window_t;
typedef struct wm {window_t *top;
    void (*paint_background)(struct wm*,surface_t*,rect_t);} wm_t;
static struct {int desktop_top,desktop_bottom;} g_theme;
static int scale=1,paints;
static uint64_t pixels,checks;
static rect_t painted[64];
#define WIN_RADIUS (7*scale)
#define WIN_MINIMISED 1
#define ANIM_NONE 0
#define BACKGROUND_REGION_MAX 64
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static bool same(rect_t a,rect_t b){return !memcmp(&a,&b,sizeof a);}
static void surface_set_clip(surface_t *s,rect_t r){s->clip=r;}
static void record(surface_t *s,rect_t r){
    assert(same(s->clip,r)&&paints<64);painted[paints++]=r;
    pixels+=(uint64_t)r.w*r.h;
}
static void callback(wm_t *wm,surface_t *s,rect_t r){(void)wm;record(s,r);}
static void gui_gradient_v(surface_t *s,rect_t full,int a,int b){
    assert(same(full,rect_make(0,0,s->width,s->height)));
    assert(a==g_theme.desktop_top&&b==g_theme.desktop_bottom);record(s,s->clip);
}
'''
    for name in ('rect_empty', 'rect_intersection'):
        c += '\n' + function(draw, name)
    for name in ('background_regions', 'paint_exposed_background'):
        c += '\n' + function(src, name)
    c += r'''
static bool inside(rect_t r,int x,int y){
    return x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;
}
static void check(wm_t *wm,rect_t area){
    struct {uint64_t left;rect_t r[64];uint64_t right;} out={.left=123,.right=456};
    int n=background_regions(wm,area,out.r);
    assert(out.left==123&&out.right==456&&n>=-1&&n<=64);
    surface_t s={.width=7680,.height=1440};
    for(int route=0;route<2;route++) {
        wm->paint_background=route?callback:NULL;paints=0;pixels=0;
        paint_exposed_background(wm,&s,area);assert(same(s.clip,area));
        assert(paints==(n<0?1:n));
        for(int i=0;i<paints;i++) {
            rect_t r=painted[i];assert(!rect_empty(r));
            assert(r.x>=area.x&&r.y>=area.y&&r.x+r.w<=area.x+area.w&&r.y+r.h<=area.y+area.h);
            for(int j=0;j<i;j++)assert(rect_empty(rect_intersection(r,painted[j])));
        }
        for(int y=area.y;y<area.y+area.h;y++)for(int x=area.x;x<area.x+area.w;x++) {
            bool exposed=true;
            for(window_t *w=wm->top;w;w=w->below) {
                if(!w->visible||w->state==WIN_MINIMISED||w->anim.kind!=ANIM_NONE)continue;
                int r=7*scale;
                if(x>=w->frame.x+r&&x<w->frame.x+w->frame.w-r&&
                   y>=w->frame.y+r&&y<w->frame.y+w->frame.h-r)exposed=false;
            }
            int hits=0;for(int i=0;i<paints;i++)hits+=inside(painted[i],x,y);
            assert(hits==(n<0?1:(int)exposed));checks++;
        }
    }
}
static uint32_t seed=0x507071;
static unsigned rnd(unsigned max){seed=seed*1664525u+1013904223u;return seed%max;}
int main(void){
    wm_t wm={0};rect_t area={0,0,96,64};
    check(&wm,area);check(&wm,(rect_t){0,0,0,64});
    window_t windows[100]={0};
    for(int n=0;n<100;n++)windows[n].below=n?&windows[n-1]:NULL;
    for(int trial=0;trial<4000;trial++) {
        int n=(int)rnd(12);wm.top=n?&windows[n-1]:NULL;scale=1+(int)rnd(3);
        for(int i=0;i<n;i++) {
            windows[i].frame=(rect_t){(int)rnd(150)-40,(int)rnd(100)-30,(int)rnd(120),(int)rnd(90)};
            windows[i].visible=rnd(5)!=0;windows[i].state=rnd(6)==0;
            windows[i].anim.kind=rnd(4)==0;
        }
        check(&wm,area);
    }
    // Fragmentation cap must repaint the original region, never leave holes.
    scale=1;
    for(int i=0;i<100;i++)windows[i]=(window_t){
        .frame={5+(i%10)*7-7,5+(i/10)*5-7,15,15},.visible=true,
        .below=i?&windows[i-1]:NULL};
    wm.top=&windows[99];rect_t parts[64];assert(background_regions(&wm,area,parts)==-1);
    check(&wm,area);
    // Actual large 3D window damage includes rounded edges and a shadow.
    // All those pixels remain, while its hidden background is excluded.
    scale=2;windows[0]=(window_t){.frame={100,100,1920,960},.visible=true};
    wm.top=&windows[0];wm.paint_background=callback;
    surface_t s={.width=7680,.height=1440};rect_t damage={72,72,1976,1020};
    paints=0;pixels=0;paint_exposed_background(&wm,&s,damage);
    uint64_t before=(uint64_t)damage.w*damage.h;
    assert(paints==4&&pixels==before-(1920-28)*(uint64_t)(960-28));
    assert(pixels<before/7); // >85% less hidden-background pixel work.
    printf("PASS background occlusion: %llu independent pixel checks, callback/gradient clipping, no overlap, empty/minimized/animated/rounded cases, bounded-fragmentation fallback; large-window background %llu -> %llu pixels (modeled painter)\n",
        (unsigned long long)checks,(unsigned long long)before,(unsigned long long)pixels);
}
'''
    repaint = function(src, 'repaint_area')
    assert 'paint_exposed_background(wm,s,area);' in repaint
    assert repaint.index('paint_exposed_background') < repaint.index('for (window_t *w')
    run_test(c, 'window_background')


if __name__ == '__main__':
    main()
