#!/usr/bin/env python3
"""Production repaint scheduling/accounting, with modeled drawing and GPU I/O.

Checks final pixels, draw-before-present order, callback damage preservation,
and redraw (not rectangle) accounting. Does not measure native GPU performance.
"""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT/'user/libgui/window.c').read_text()
    draw = (ROOT/'user/libgui/draw.c').read_text()
    paint = function(src, 'repaint_area')
    assert 'display_present(' not in paint
    assert 'frame_count++' not in paint
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define WM_MAX_DAMAGE 12
typedef struct {int x,y,w,h;} rect_t;
typedef struct {int width,height;void *gpu;} surface_t;
typedef struct {surface_t *back;} display_t;
typedef struct {display_t *display;rect_t damage[WM_MAX_DAMAGE];int damage_count;bool full_redraw;} wm_t;
static uint64_t frame_total_us,frame_count,frame_pixels,phase_present_us;
static uint64_t clock_us;
static unsigned painted,presented,pending,flushes,done,colour;
static int request_next;
static rect_t areas_seen[WM_MAX_DAMAGE];
static unsigned back[64*48],screen[64*48],expected[64*48];
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static uint64_t uptime_us(void){return clock_us;}
static void log_write(int level,const char *tag,const char *line){(void)level;(void)tag;(void)line;}
static bool same(rect_t a,rect_t b){return !memcmp(&a,&b,sizeof a);}
'''
    code += function(draw, 'rect_empty') + function(draw, 'rect_intersection')
    code += r'''
static void repaint_area(wm_t *wm,rect_t a){
    assert(!presented&&painted<WM_MAX_DAMAGE);areas_seen[painted++]=a;
    assert(!rect_empty(a)&&a.x>=0&&a.y>=0&&a.x+a.w<=64&&a.y+a.h<=48);
    for(int y=a.y;y<a.y+a.h;y++)for(int x=a.x;x<a.x+a.w;x++)back[y*64+x]=colour;
    frame_pixels+=(uint64_t)a.w*a.h;pending++;clock_us+=10;
    if(painted==1&&request_next){
        assert(!wm->full_redraw&&!wm->damage_count);
        if(request_next==2)wm->full_redraw=true;
        else wm->damage[wm->damage_count++]=(rect_t){60,40,4,4};
    }
}
static void display_present(display_t *d,rect_t a){
    assert(d&&presented<painted&&same(a,areas_seen[presented]));
    if(pending){flushes++;pending=0;}
    for(int y=a.y;y<a.y+a.h;y++)for(int x=a.x;x<a.x+a.w;x++)screen[y*64+x]=back[y*64+x];
    presented++;clock_us+=5;
}
static void blit_present_done(void){done++;clock_us+=7;}
'''
    code += function(src, 'take_repaint_damage') + function(src, 'repaint')
    code += r'''
static unsigned seed=0x507071;
static unsigned rnd(unsigned n){seed=seed*1664525u+1013904223u;return seed%n;}
static void check(wm_t *wm){
    rect_t initial[WM_MAX_DAMAGE];int count=0;
    if(wm->full_redraw)initial[count++]=(rect_t){0,0,64,48};
    else for(int i=0;i<wm->damage_count;i++){
        rect_t a=rect_intersection(wm->damage[i],(rect_t){0,0,64,48});
        if(!rect_empty(a))initial[count++]=a;
    }
    memcpy(expected,screen,sizeof screen);uint64_t pixels=0;
    for(int i=0;i<count;i++){
        rect_t a=initial[i];pixels+=(uint64_t)a.w*a.h;
        for(int y=a.y;y<a.y+a.h;y++)for(int x=a.x;x<a.x+a.w;x++)expected[y*64+x]=colour;
    }
    frame_count=frame_total_us=frame_pixels=phase_present_us=0;
    painted=presented=pending=flushes=done=0;
    repaint(wm);
    assert(painted==(unsigned)count&&presented==(unsigned)count);
    assert(!memcmp(screen,expected,sizeof screen)&&!pending);
    assert(frame_count==(count?1u:0u)&&frame_pixels==pixels);
    assert(frame_total_us==(count?(uint64_t)count*15+7:0));
    assert(phase_present_us==(count?(uint64_t)count*5+7:0));
    assert(done==(count?1u:0u)&&flushes==(count?1u:0u));
    if(count&&request_next){
        assert(wm->full_redraw==(request_next==2));
        assert(wm->damage_count==(request_next==1));
    }else assert(!wm->full_redraw&&!wm->damage_count);
    for(int i=0;i<count;i++)assert(same(initial[i],areas_seen[i]));
}
int main(void){
    surface_t s={64,48,NULL};display_t d={&s};wm_t wm={.display=&d};
    for(int route=0;route<2;route++){
        s.gpu=route?(void*)1:NULL;
        for(int trial=0;trial<2000;trial++){
            colour=trial+1;request_next=trial%3;
            wm.full_redraw=rnd(8)==0;wm.damage_count=rnd(WM_MAX_DAMAGE+1);
            for(int i=0;i<wm.damage_count;i++)wm.damage[i]=(rect_t){(int)rnd(100)-20,(int)rnd(80)-20,(int)rnd(70),(int)rnd(55)};
            check(&wm);
            // A callback's invalidation must really render on the next pump.
            request_next=0;colour+=3000;check(&wm);
        }
    }
    puts("PASS 8000 production repaint batches: exact clipped pixels/order, all painting before present, callback damage retained, full/empty/max regions, one redraw count including final flush; GPU I/O modeled, not native timing");
}
'''
    run_test(code, 'window_present_batch')


if __name__ == '__main__':
    main()
