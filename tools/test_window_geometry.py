#!/usr/bin/env python3
"""Actual desktop/window geometry functions with allocation/driver fault edges."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT/'user/libgui/window.c').read_text()
    # Native ABI is LP64; Windows host long is 32 bits. Preserve pointer width
    # at the mocked syscall boundary without changing native control flow.
    display = (ROOT/'user/libgui/display.c').read_text().replace('(long)&info', '(intptr_t)&info')
    code = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t colour_t;
typedef struct{int x,y,w,h;}rect_t;
typedef struct{int width,height,stride;colour_t*pixels;void*gpu;}surface_t;
typedef struct{uint64_t address;uint32_t width,height,pitch,bpp,red_shift,green_shift,blue_shift,size;}kframebuffer_t;
typedef struct{surface_t screen,*back;kframebuffer_t info;bool driver_updates,hardware_cursor;}display_t;
typedef struct{uint32_t pixels[64*64];int width,height,hot_x,hot_y;}kcursor_t;
typedef enum{WIN_NORMAL,WIN_MINIMISED,WIN_MAXIMISED}win_state;
enum{ANIM_NONE,ANIM_OPEN,ANIM_CLOSE,ANIM_MINIMISE,ANIM_RESTORE,ANIM_GEOMETRY};
enum{WE_RESIZE=10};
typedef struct{int kind,x,y;}wevent_t;
typedef struct wm wm_t;
typedef struct window{
    rect_t frame,restore;surface_t*canvas;win_state state;wm_t*wm;
    struct window*above;struct{int kind;}anim;bool needs_paint,resizable;
    int min_w,min_h;
}window_t;
struct wm{
    display_t*display;window_t*bottom,*drag,*resize;int resize_edge,mouse_x,mouse_y;
    rect_t work_area;bool animating,animations_on;
    rect_t(*query_work_area)(wm_t*,int,int);
    void(*on_resize)(wm_t*,int,int);
    rect_t(*taskbar_slot)(wm_t*,window_t*);
};
#define WIN_BORDER 1
#define WIN_TITLE_H (32*scale)
#define WM_MAX_WINDOWS 24
#define SYS_FRAMEBUFFER 1
#define ANIM_ARRIVE_ALPHA 255
#define ANIM_MOVE_MS 160
static int scale=1,allocs,frees,fail_at,driver_fail,map_fail,mode_calls,map_calls;
static int events,damage,notifications,animations,focuses,requested_w,requested_h;
static bool hardware_touched,all_windows_consistent;
static display_t d;
static wm_t wm;
static window_t windows[WM_MAX_WINDOWS];
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static bool same(rect_t a,rect_t b){return !memcmp(&a,&b,sizeof a);}
static void log_write(int level,const char*a,const char*b){(void)level;(void)a;(void)b;}
static surface_t*surface_create_target(int w,int h,bool gpu){
    assert(w>0&&h>0);assert(!hardware_touched); /* no post-modeset allocation */
    if(++allocs==fail_at)return NULL;
    surface_t*s=calloc(1,sizeof*s);assert(s);s->width=w;s->height=h;s->gpu=gpu?(void*)1:NULL;return s;
}
static void surface_destroy(surface_t*s){assert(s);frees++;free(s);}
static int fb_set_mode(unsigned w,unsigned h){
    mode_calls++;assert(allocs>0);requested_w=w;requested_h=h;
    if(driver_fail){errno=ENOSYS;return -1;}hardware_touched=true;return 0;
}
static long syscall6(long op,intptr_t address,long a,long b,long c,long e,long f){
    (void)a;(void)b;(void)c;(void)e;(void)f;assert(op==SYS_FRAMEBUFFER);map_calls++;
    if(map_fail)return -1;
    *(kframebuffer_t*)address=(kframebuffer_t){.address=0x12345000,.width=requested_w,.height=requested_h,
        .pitch=requested_w*4+64,.bpp=32,.size=(requested_w*4+64)*requested_h,.red_shift=16,.green_shift=8};return 0;
}
static void gui_cursor_image(uint32_t*p,int*w,int*h,int*x,int*y){(void)p;*w=*h=16;*x=*y=0;}
static int fb_set_cursor(kcursor_t*p){assert(p->width==16);return 0;}
static void wm_damage(wm_t*m,rect_t r){(void)m;(void)r;damage++;}
static void wm_damage_all(wm_t*m){(void)m;damage++;}
static rect_t window_visual_bounds(rect_t r){return r;}
static void consistent(wm_t*m){
    for(window_t*w=m->bottom;w;w=w->above){
        assert(w->canvas&&w->canvas->width==w->frame.w-2*WIN_BORDER);
        assert(w->canvas->height==w->frame.h-WIN_TITLE_H-WIN_BORDER);
    }
}
static bool win_proc(window_t*w,const wevent_t*ev){
    assert(ev->kind==WE_RESIZE&&ev->x==w->canvas->width&&ev->y==w->canvas->height);
    if(all_windows_consistent)consistent(w->wm);events++;return false;
}
static void wm_focus(wm_t*m,window_t*w){(void)m;(void)w;focuses++;}
static void wm_animate(wm_t*m,window_t*w,int kind,rect_t from,rect_t to,int a,int b,int ms){
    (void)m;(void)from;(void)a;(void)b;(void)ms;assert(same(to,w->frame));w->anim.kind=kind;animations++;
}
static rect_t query(wm_t*m,int w,int h){(void)m;return rect_make(8,16,w-24,h-64);}
static void notified(wm_t*m,int w,int h){
    assert(m->display->back->width==w&&m->display->back->height==h);
    assert(same(m->work_area,query(m,w,h)));consistent(m);notifications++;
}
'''
    code += function(display, 'display_set_mode')+'\n'
    for name in ['wm_is_animating', 'window_prepare_geometry', 'window_set_geometry',
                 'window_fit_area', 'wm_set_mode', 'wm_restore', 'wm_toggle_maximise',
                 'snap_to_rect', 'apply_resize']:
        code += function(src, name)+'\n'
    code += r'''
static void reset(int n,bool gpu){
    memset(&d,0,sizeof d);memset(&wm,0,sizeof wm);memset(windows,0,sizeof windows);
    hardware_touched=false;allocs=frees=fail_at=driver_fail=map_fail=0;
    events=damage=notifications=animations=focuses=mode_calls=map_calls=0;
    all_windows_consistent=true;
    d.back=surface_create_target(1920,1080,gpu);d.screen.width=1920;d.screen.height=1080;
    d.info.width=1920;d.info.height=1080;d.driver_updates=true;d.hardware_cursor=true;
    wm.display=&d;wm.work_area=query(&wm,1920,1080);wm.query_work_area=query;wm.on_resize=notified;
    wm.bottom=n?windows:NULL;wm.mouse_x=1900;wm.mouse_y=-20;
    for(int i=0;i<n;i++){
        window_t*w=&windows[i];w->wm=&wm;w->resizable=true;w->min_w=120;w->min_h=60;
        w->frame=rect_make(900+i,600+i,1000,800);w->restore=w->frame;
        w->canvas=surface_create_target(998,800-WIN_TITLE_H-WIN_BORDER,gpu);
        w->state=i%3==0?WIN_MAXIMISED:WIN_NORMAL;
        if(i+1<n)w->above=&windows[i+1];
    }
    allocs=frees=0;
}
static void cleanup(void){
    for(window_t*w=wm.bottom;w;w=w->above)surface_destroy(w->canvas);
    surface_destroy(d.back);
}
static void batches(void){
    for(int gpu=0;gpu<2;gpu++)for(int n=0;n<=WM_MAX_WINDOWS;n++){
        for(int fail=1;fail<=n+1;fail++){
            reset(n,gpu);window_t saved[WM_MAX_WINDOWS];memcpy(saved,windows,sizeof saved);
            wm_t oldwm=wm;display_t oldd=d;fail_at=fail;
            assert(!wm_set_mode(&wm,800,600));
            assert(!memcmp(saved,windows,sizeof saved)&&!memcmp(&wm,&oldwm,sizeof wm)&&!memcmp(&d,&oldd,sizeof d));
            assert(!mode_calls&&!events&&!damage&&!notifications&&frees==fail-1);cleanup();
        }
        for(int failed_driver=1;failed_driver<=2;failed_driver++){
            reset(n,gpu);window_t saved[WM_MAX_WINDOWS];memcpy(saved,windows,sizeof saved);
            wm_t oldwm=wm;display_t oldd=d;driver_fail=failed_driver==1;map_fail=failed_driver==2;
            assert(!wm_set_mode(&wm,800,600));
            assert(!memcmp(saved,windows,sizeof saved)&&!memcmp(&wm,&oldwm,sizeof wm)&&!memcmp(&d,&oldd,sizeof d));
            assert(mode_calls==1&&map_calls==(failed_driver==2)&&frees==n+1&&!events&&!notifications);cleanup();
        }
        reset(n,gpu);
        assert(wm_set_mode(&wm,800,600));consistent(&wm);
        assert(events==n&&notifications==1&&mode_calls==1&&map_calls==1&&frees==n+1);
        assert(wm.mouse_x==799&&wm.mouse_y==0&&!wm.drag&&!wm.resize);
        for(int i=0;i<n;i++){
            window_t*w=&windows[i];assert(w->frame.x>=8&&w->frame.y>=16);
            assert(w->frame.x+w->frame.w<=784&&w->frame.y+w->frame.h<=552);
            assert(w->restore.x+w->restore.w<=784&&w->restore.y+w->restore.h<=552);
            assert((w->canvas->gpu!=NULL)==(gpu!=0));
        }
        cleanup();
    }
    reset(1,true);wm.query_work_area=NULL;assert(wm_set_mode(&wm,800,600));
    assert(same(wm.work_area,query(&wm,800,600)));cleanup();
    reset(1,true);assert(wm_set_mode(&wm,1920,1080)&&!allocs&&!mode_calls&&!events);cleanup();
}
static void individual(void){
    for(scale=1;scale<=4;scale++)for(int gpu=0;gpu<2;gpu++)for(int op=0;op<5;op++)for(int fail=0;fail<2;fail++){
        reset(1,gpu);window_t*w=windows;w->state=WIN_NORMAL;
        w->restore=rect_make(20,30,640,480);
        if(op==1)w->state=WIN_MAXIMISED;
        window_t before=*w;wm.animations_on=true;fail_at=fail?1:0;
        wm.resize=w;wm.resize_edge=10;
        if(op==0||op==1)wm_toggle_maximise(&wm,w);
        if(op==2){w->state=WIN_MINIMISED;before=*w;wm_restore(&wm,w);}
        if(op==3)snap_to_rect(&wm,w,rect_make(0,0,700,700));
        if(op==4)apply_resize(&wm,35,17);
        if(fail){assert(!memcmp(&before,w,sizeof before)&&!events&&!damage&&!animations&&!focuses);}
        else{consistent(&wm);assert(events==1&&frees==1&&damage>0);}
        cleanup();
    }
    scale=1;reset(1,true);window_t*w=windows;window_t before=*w;
    assert(!window_set_geometry(w,rect_make(0,0,1,1),w->restore,w->state));
    assert(!memcmp(&before,w,sizeof before)&&!allocs&&!events&&!damage);
    rect_t f=w->frame;f.x+=10;f.y-=20;
    assert(window_set_geometry(w,f,w->restore,w->state)&&!allocs&&!events);cleanup();
}
int main(void){batches();individual();puts("PASS: actual desktop/window preallocation, 0-24 windows on CPU/GPU targets, every allocation/driver/map failure, work-area/restore bounds, callback ordering, scaled maximize/restore/snap/drag resize");}
'''
    run_test(code, 'window_geometry')
    assert 'resize_canvas(' not in src
    assert 'g_wm.query_work_area = query_work_area;' in (ROOT/'user/desktop/desktop.c').read_text()


if __name__ == '__main__':
    main()
