#!/usr/bin/env python3
"""Execute actual window-move/damage code for scaled shadow footprints."""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]

def main():
    src = (ROOT / 'user/libgui/window.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,w,h;} rect_t;
typedef struct {int width,height;bool gpu;} surface_t;
typedef struct {surface_t *back;} display_t;
typedef struct {rect_t frame;} window_t;
#define WM_MAX_DAMAGE 16
#define WIN_TITLE_H 32
typedef struct {display_t *display;rect_t work_area,damage[WM_MAX_DAMAGE];int damage_count;bool full_redraw;} wm_t;
static int scale=1;
static int gui_screen_scale(void){return scale;}
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static bool rect_empty(rect_t r){return r.w<=0||r.h<=0;}
static bool contains(rect_t r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;}
static bool rect_intersects(rect_t a,rect_t b){return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;}
static rect_t rect_union(rect_t a,rect_t b){int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;return rect_make(x,y,r-x,d-y);}
static rect_t move_on_adapter(wm_t *wm,window_t *w,rect_t a,rect_t b){
    (void)w;(void)a;(void)b;assert(wm->display->back->gpu);return rect_make(0,0,0,0);}
static void damage_except(wm_t *wm,rect_t a,rect_t b){(void)wm;(void)a;(void)b;assert(false);}
'''
    for name in ['merge_waste','wm_damage','window_visual_bounds','window_reaches_area','wm_move_window']:
        c += '\n' + function(src,name)
    c += r'''
static void covered(wm_t *wm,rect_t f){
    /* Independent drawing footprint from paint_frame_at/gui_soft_shadow. */
    int spread=14*scale;
    for(int y=f.y-spread+2;y<f.y+f.h+spread+4;y++)
        for(int x=f.x-spread;x<f.x+f.w+spread;x++){
            bool found=wm->full_redraw;
            for(int i=0;i<wm->damage_count;i++)found|=contains(wm->damage[i],x,y);
            assert(found);
        }
}
int main(void){
    surface_t back={2048,1440,true};display_t display={&back};
    for(scale=1;scale<=4;scale++)for(int dx=-61;dx<=61;dx+=61)for(int dy=-41;dy<=41;dy+=41){
        if(!dx&&!dy)continue;
        wm_t wm={.display=&display,.work_area={0,0,2048,1400}};
        window_t w={.frame={200,170,320,190}};rect_t old=w.frame;
        wm_move_window(&wm,&w,old.x+dx,old.y+dy);covered(&wm,old);covered(&wm,w.frame);
        int spread=14*scale;
        rect_t shadow_only=rect_make(w.frame.x-spread/2,w.frame.y+50,1,10);
        assert(!rect_intersects(w.frame,shadow_only));
        assert(window_reaches_area(w.frame,shadow_only));
        assert(!window_reaches_area(w.frame,rect_make(w.frame.x-spread-2,w.frame.y,1,1)));
    }
    puts("PASS actual move/damage: scaled old/new shadow footprints and shadow-only repaint reach at scales 1-4 (modeled display)");
}
'''
    assert 'else if (window_reaches_area(w->frame, area))' in src
    run_test(c,'window_damage')

if __name__ == '__main__': main()
