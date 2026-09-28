#!/usr/bin/env python3
"""Run real desktop hover/layout and WM damage code; no GPU timing claim."""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    desktop = (ROOT / 'user/desktop/desktop.c').read_text()
    window = (ROOT / 'user/libgui/window.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
typedef struct {int x,y,w,h;} rect_t;
typedef struct {int width,height;} surface_t;
typedef struct {surface_t *back;} display_t;
typedef struct window {rect_t frame;bool visible,dying;int state;struct window *below;} window_t;
#define WM_MAX_WINDOWS 24
#define WM_MAX_DAMAGE 16
#define WIN_MINIMISED 1
#define FONT_UI 0
typedef struct {display_t *display;window_t *top;int mouse_x,mouse_y;
    rect_t damage[WM_MAX_DAMAGE];int damage_count;bool full_redraw;} wm_t;
typedef struct {int x,y;} kinput_event_t;
typedef struct {bool on_desktop;} app_entry_t;
static app_entry_t apps[8]={{true},{true},{true},{true}};
static display_t g_display;
static int scale=1,task_windows=3,task_hover=-1,menu_hover=-1,desk_hover=-1;
static bool menu_open;
static unsigned steps;
static int gui_scale(void){return scale;}
static int gui_text_width(int font,const char *text){(void)font;(void)text;return 32*scale;}
static const app_entry_t *desktop_apps(int *count){*count=8;return apps;}
static rect_t rect_make(int x,int y,int w,int h){return (rect_t){x,y,w,h};}
static bool rect_empty(rect_t r){return r.w<=0||r.h<=0;}
static bool rect_contains(rect_t r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;}
static rect_t rect_union(rect_t a,rect_t b){int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
return rect_make(x,y,r-x,d-y);}
'''
    for name in ['TASKBAR_H', 'LAUNCHER_W', 'MENU_W', 'MENU_ITEM_H', 'MENU_HEADER_H',
                 'DESK_ICON_W', 'DESK_ICON_H']:
        c += re.search(r'^#define\s+' + name + r'\s+[^\n]*', desktop, re.M)[0] + '\n'
    for name in ['merge_waste', 'wm_damage']:
        c += '\n' + function(window, name)
    start = window.index('window_t *wm_window_at(')
    c += '\n' + window[start:window.index('\n}', start) + 2]
    for name in ['taskbar_rect', 'launcher_rect', 'power_rect', 'menu_rect',
                 'menu_item_rect', 'task_button_span', 'task_button_rect',
                 'desktop_icon_rect', 'desktop_icon_count', 'desktop_update_hover']:
        c += '\n' + function(desktop, name)
    c += r'''
static void covered(wm_t *wm,rect_t r) {
    for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++) {
        bool found=false;
        for(int i=0;i<wm->damage_count;i++)found|=rect_contains(wm->damage[i],x,y);
        assert(found);
    }
}
static void motion(wm_t *wm,int x,int y,int task,int menu,int desk) {
    int old_t=task_hover,old_m=menu_hover,old_d=desk_hover;
    bool old_l=rect_contains(launcher_rect(),wm->mouse_x,wm->mouse_y);
    bool old_p=rect_contains(power_rect(),wm->mouse_x,wm->mouse_y);
    bool new_l=rect_contains(launcher_rect(),x,y),new_p=rect_contains(power_rect(),x,y);
    wm->damage_count=0;wm->full_redraw=false;
    kinput_event_t ev={x,y};desktop_update_hover(wm,&ev);
    assert(task_hover==task&&menu_hover==menu&&desk_hover==desk);
    assert(!wm->full_redraw);
    if(task!=old_t){if(old_t>=0)covered(wm,task_button_rect(old_t));if(task>=0)covered(wm,task_button_rect(task));}
    if(menu!=old_m){if(old_m>=0)covered(wm,menu_item_rect(old_m));if(menu>=0)covered(wm,menu_item_rect(menu));}
    if(desk!=old_d){if(old_d>=0)covered(wm,desktop_icon_rect(old_d));if(desk>=0)covered(wm,desktop_icon_rect(desk));}
    if(!menu_open&&old_l!=new_l)covered(wm,launcher_rect());
    if(old_p!=new_p)covered(wm,power_rect());
    bool changed=task!=old_t||menu!=old_m||desk!=old_d||(!menu_open&&old_l!=new_l)||old_p!=new_p;
    if(!changed)assert(!wm->damage_count);
    uint64_t pixels=0;
    for(int i=0;i<wm->damage_count;i++)pixels+=(uint64_t)wm->damage[i].w*wm->damage[i].h;
    assert(pixels<(uint64_t)g_display.back->width*g_display.back->height/4);
    wm->mouse_x=x;wm->mouse_y=y;steps++;
}
static void visit(wm_t *wm,rect_t r,int task,int menu,int desk){
    motion(wm,r.x+r.w/2,r.y+r.h/2,task,menu,desk);
}
static void blank(wm_t *wm){motion(wm,g_display.back->width/2,g_display.back->height/2,-1,-1,-1);}
int main(void){
    for(int layout=0;layout<5;layout++) {
        scale=layout==4?2:layout+1;
        surface_t back={layout==4?7680:1920*scale,layout==4?1440:1080*scale};
        g_display.back=&back;
        wm_t wm={.display=&g_display,.mouse_x=back.width/2,.mouse_y=back.height/2};
        task_hover=menu_hover=desk_hover=-1;menu_open=false;
        visit(&wm,desktop_icon_rect(0),-1,-1,0);
        visit(&wm,desktop_icon_rect(0),-1,-1,0); // no repeated repaint
        visit(&wm,desktop_icon_rect(1),-1,-1,1);blank(&wm);
        visit(&wm,task_button_rect(0),0,-1,-1);
        visit(&wm,task_button_rect(0),0,-1,-1);
        visit(&wm,task_button_rect(1),1,-1,-1);blank(&wm);
        visit(&wm,task_button_rect(task_windows),-1,-1,-1);blank(&wm); // no invisible task button
        visit(&wm,launcher_rect(),-1,-1,-1);
        visit(&wm,launcher_rect(),-1,-1,-1);blank(&wm);
        visit(&wm,power_rect(),-1,-1,-1);
        visit(&wm,power_rect(),-1,-1,-1);blank(&wm);
        menu_open=true;
        visit(&wm,menu_item_rect(0),-1,0,-1);
        visit(&wm,menu_item_rect(0),-1,0,-1);
        visit(&wm,menu_item_rect(1),-1,1,-1);blank(&wm);
        visit(&wm,launcher_rect(),-1,-1,-1);blank(&wm); // open launcher remains highlighted
        menu_open=false;
        visit(&wm,desktop_icon_rect(0),-1,-1,0);
        window_t cover={.frame=desktop_icon_rect(0),.visible=true};wm.top=&cover;
        visit(&wm,desktop_icon_rect(0),-1,-1,-1); // hidden icon highlight restored
        cover.visible=false;visit(&wm,desktop_icon_rect(0),-1,-1,0);
        cover.visible=true;cover.state=WIN_MINIMISED;visit(&wm,desktop_icon_rect(0),-1,-1,0);
        wm.top=0;blank(&wm);
    }
    printf("PASS actual desktop hover: %u transitions, scales 1-4 plus 7680x1440; old/new highlight coverage, no repeated/full redraw, launcher/power and window/menu occlusion (host damage model)\n",steps);
}
'''
    intercept = function(desktop, 'desktop_intercept')
    assert 'desktop_update_hover(wm, ev);' in intercept
    run_test(c, 'desktop_hover_damage')


if __name__ == '__main__':
    main()
