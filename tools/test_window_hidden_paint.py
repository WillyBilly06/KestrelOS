#!/usr/bin/env python3
"""Run actual WM paint scheduling with synthetic windows, not GPU performance."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT / 'user/libgui/window.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { int x,y,w,h; } rect_t;
typedef struct { int width,height; } surface_t;
typedef struct { surface_t *back; } display_t;
typedef struct { int kind; } wevent_t;
typedef struct { int unused; } kinput_event_t;
enum { ANIM_NONE, ANIM_MOVE, WIN_MINIMISED, WE_PAINT, WE_TICK };
#define WIN_RADIUS 8
#define TICK_MS 120
typedef struct window {
    rect_t frame; struct window *above;
    bool visible,continuous,needs_paint; int state,proc,paints,ticks;
    struct { int kind; } anim; surface_t *canvas;
} window_t;
typedef struct wm wm_t;
struct wm {
    display_t *display; window_t *bottom; bool running,animating,drawing_continuously;
    bool (*background_event)(wm_t *, kinput_event_t *);
    void (*on_tick)(wm_t *);
};
static int damage,compositions; static uint64_t now;
static int gui_screen_scale(void){return 1;}
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static bool rect_empty(rect_t r){return r.w<=0||r.h<=0;}
static bool rect_intersects(rect_t a,rect_t b){return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;}
static rect_t rect_intersection(rect_t a,rect_t b){
    int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r-x,d-y);
}
static bool display_poll_event(display_t *d,kinput_event_t *e){(void)d;(void)e;return false;}
static void handle_event(wm_t *w,kinput_event_t *e){(void)w;(void)e;assert(false);}
static void stage_retire_if_idle(wm_t *w){(void)w;}
static void wm_damage(wm_t *w,rect_t r){(void)w;assert(!rect_empty(r));damage++;}
static void wm_damage_all(wm_t *w){(void)w;damage++;}
static bool wm_has_damage(wm_t *w){(void)w;return damage!=0;}
static void repaint(wm_t *w){(void)w;compositions++;damage=0;}
static void surface_reset_clip(surface_t *s){assert(s);}
static bool step_animations(wm_t *w){(void)w;return false;}
static uint64_t uptime_ms(void){return now;}
static bool win_proc(window_t *w,wevent_t *e){
    if(e->kind==WE_PAINT)w->paints++;
    if(e->kind==WE_TICK){w->ticks++;return true;}
    return false;
}
'''
    for name in ('rect_covers', 'window_visual_bounds', 'window_reaches_area',
                 'window_hidden_in', 'window_paint_visible', 'wm_invalidate', 'wm_pump'):
        c += function(src, name) + '\n'
    c += r'''
int main(void){
    surface_t screen={1000,800};display_t d={&screen};
    window_t cover={.frame={40,40,400,350},.visible=true,.canvas=&screen};
    window_t app={.frame={100,100,200,150},.visible=true,.continuous=true,
                  .proc=1,.canvas=&screen,.above=&cover};
    wm_t wm={.display=&d,.bottom=&app,.running=true};
    wm_invalidate(&wm,&app);assert(app.needs_paint&&!damage);
    // Covered render callback stays pending; ticks still run, no invisible
    // command construction/GPU submissions/composition/uncapped frame loop.
    for(int i=0;i<1000;i++){
        now+=120;wm_pump(&wm);
        assert(!app.paints&&!compositions&&!wm.drawing_continuously&&app.needs_paint);
    }
    assert(app.ticks==1000);
    cover.visible=false;now++;wm_pump(&wm);
    assert(app.paints==1&&compositions==1&&wm.drawing_continuously&&!app.needs_paint);
    // No artificial pacing of a visible continuous app.
    for(int i=0;i<100;i++){now++;wm_pump(&wm);}
    assert(app.paints==101&&compositions==101);
    cover.visible=true;app.continuous=false;wm_invalidate(&wm,&app);
    int n=app.paints;now++;wm_pump(&wm);assert(app.paints==n&&app.needs_paint);
    // Moving, minimizing or closing the cover exposes current pending content.
    cover.frame.x=700;now++;wm_pump(&wm);assert(app.paints==n+1&&!app.needs_paint);
    cover.frame.x=40;cover.state=WIN_MINIMISED;assert(window_paint_visible(&wm,&app));
    cover.state=0;cover.anim.kind=ANIM_MOVE;assert(window_paint_visible(&wm,&app));
    cover.anim.kind=ANIM_NONE;
    // One exposed shadow edge prevents culling. Rounded corners are not opaque.
    cover.frame.x=app.frame.x-14;assert(window_paint_visible(&wm,&app));
    cover.frame.x=40;assert(!window_paint_visible(&wm,&app));
    app.anim.kind=ANIM_MOVE;assert(window_paint_visible(&wm,&app));app.anim.kind=ANIM_NONE;
    app.state=WIN_MINIMISED;assert(!window_paint_visible(&wm,&app));app.state=0;
    app.frame.x=1100;assert(!window_paint_visible(&wm,&app));
    app.frame.x=995;assert(window_paint_visible(&wm,&app));
    app.visible=false;assert(!window_paint_visible(&wm,&app));
    puts("PASS actual WM pump: 1000 covered ticks without paints/submits/redraws, pending repaint restored on reveal, 100 unpaced visible frames, conservative animation/shadow/rounded/minimized/offscreen handling");
}
'''
    run_test(c, 'window_hidden_paint')


if __name__ == '__main__':
    main()
