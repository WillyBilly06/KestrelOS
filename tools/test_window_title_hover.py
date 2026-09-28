#!/usr/bin/env python3
"""Check actual scaled title hit tests and changed-highlight damage."""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / 'user/libgui/window.c').read_text()
    header = (ROOT / 'user/libgui/window.h').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef struct {int x,y,w,h;} rect_t;
typedef struct window {rect_t frame;bool visible,dying,closable,resizable;int state;
    struct {int kind;} anim;struct window *below;} window_t;
#define WIN_NORMAL 0
#define WIN_MINIMISED 1
#define ANIM_NONE 0
#define WM_MAX_DAMAGE 16
typedef struct {window_t *top;rect_t damage[WM_MAX_DAMAGE];int damage_count;bool full_redraw;} wm_t;
static int scale;
static unsigned long pixels,transitions;
static int gui_scale(void){return scale;}
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static bool rect_contains(rect_t r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;}
static rect_t rect_union(rect_t a,rect_t b){int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
return rect_make(x,y,r-x,d-y);}
'''
    for name in ['WIN_TITLE_H', 'WIN_BORDER', 'WIN_RESIZE_GRIP']:
        c += re.search(r'^#define\s+' + name + r'\s+[^\n]*', header, re.M)[0] + '\n'
    c += re.search(r'^enum \{ HIT_NONE[^\n]*', src, re.M)[0] + '\n'
    start = src.index('window_t *wm_window_at(')
    c += src[start:src.index('\n}', start) + 2]
    for name in ['button_rect_at', 'button_rect', 'hit_test', 'merge_waste', 'wm_damage',
                 'title_hover_rect', 'damage_title_hover']:
        c += '\n' + function(src, name)
    c += r'''
static rect_t drawn_button(rect_t f,int slot) {
    // Independently preserve the painter's original scaled geometry.
    return rect_make(f.x+f.w-1-7*scale-(slot+1)*25*scale,
                     f.y+6*scale,20*scale,20*scale);
}
static bool same(rect_t a,rect_t b){return a.x==b.x&&a.y==b.y&&a.w==b.w&&a.h==b.h;}
static void covered(wm_t *wm,rect_t r){
    for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++) {
        bool found=false;for(int i=0;i<wm->damage_count;i++)found|=rect_contains(wm->damage[i],x,y);
        assert(found);
    }
}
static void move(wm_t *wm,int ox,int oy,int x,int y,rect_t old,rect_t now) {
    wm->damage_count=0;wm->full_redraw=false;
    assert(same(title_hover_rect(wm,ox,oy),old));
    assert(same(title_hover_rect(wm,x,y),now));
    damage_title_hover(wm,ox,oy,x,y);
    assert(!wm->full_redraw);
    if(same(old,now))assert(!wm->damage_count);
    else {covered(wm,old);covered(wm,now);}
    unsigned area=0;
    for(int i=0;i<wm->damage_count;i++)area+=wm->damage[i].w*wm->damage[i].h;
    assert(area<=800u*scale*scale+4096u);
    transitions++;
}
int main(void) {
    rect_t none={0};
    for(scale=1;scale<=4;scale++)for(unsigned flags=0;flags<4;flags++) {
        window_t w={.frame={80*scale,100*scale,600*scale,400*scale},.visible=true,
            .closable=(flags&1)!=0,.resizable=(flags&2)!=0};
        wm_t wm={.top=&w};
        rect_t buttons[3];int hits[3],count=0;
        if(w.closable){buttons[count]=drawn_button(w.frame,count);hits[count++]=HIT_CLOSE;}
        if(w.resizable){buttons[count]=drawn_button(w.frame,count);hits[count++]=HIT_MAX;}
        buttons[count]=drawn_button(w.frame,count);hits[count++]=HIT_MIN;
        for(int i=0;i<count;i++)assert(same(button_rect(&w,i),buttons[i]));
        for(int y=w.frame.y;y<w.frame.y+32*scale;y++)for(int x=w.frame.x;x<w.frame.x+w.frame.w;x++) {
            int expected=HIT_NONE,edge=0;
            for(int i=0;i<count;i++)if(rect_contains(buttons[i],x,y))expected=hits[i];
            int got=hit_test(&wm,&w,x,y,&edge);
            if(expected!=HIT_NONE)assert(got==expected);
            else assert(got!=HIT_CLOSE&&got!=HIT_MAX&&got!=HIT_MIN);
            pixels++;
        }
        for(int i=0;i<count;i++) {
            rect_t b=buttons[i];int x=b.x+b.w/2,y=b.y+b.h/2;
            move(&wm,0,0,x,y,none,b);
            move(&wm,x,y,x+1,y+1,b,b);
            move(&wm,x,y,w.frame.x+50*scale,w.frame.y+80*scale,b,none);
            if(i+1<count){rect_t n=buttons[i+1];move(&wm,x,y,n.x+1,n.y+1,b,n);}
        }
        rect_t b=buttons[0];
        w.anim.kind=1;move(&wm,0,0,b.x+1,b.y+1,none,none);w.anim.kind=0;
        w.visible=false;move(&wm,0,0,b.x+1,b.y+1,none,none);w.visible=true;
        w.dying=true;move(&wm,0,0,b.x+1,b.y+1,none,none);w.dying=false;
        w.state=WIN_MINIMISED;move(&wm,0,0,b.x+1,b.y+1,none,none);w.state=WIN_NORMAL;
        window_t cover={.frame=b,.visible=true,.below=&w};wm.top=&cover;
        move(&wm,0,0,b.x+1,b.y+1,none,none);wm.top=&w;
        window_t other=w;other.frame.x+=800*scale;other.below=&w;wm.top=&other;
        rect_t second=drawn_button(other.frame,0);
        move(&wm,b.x+1,b.y+1,second.x+1,second.y+1,b,second);
    }
    printf("PASS actual title controls: %lu scaled title pixels, %lu hover transitions; painter/hit geometry, old/new damage, no repeated strip redraw and occlusion/animation gates\n",pixels,transitions);
}
'''
    painter = function(src, 'paint_frame_at')
    assert painter.count('button_rect_at(f, index)') == 3
    events = function(src, 'handle_event')
    assert 'damage_title_hover(wm, old_x, old_y, ev->x, ev->y)' in events
    run_test(c, 'window_title_hover')


if __name__ == '__main__':
    main()
