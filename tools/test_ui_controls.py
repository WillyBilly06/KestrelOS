#!/usr/bin/env python3
"""Run shared scrollbar bounds and stable typography code on the host."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    widget = (ROOT/'user/libgui/widgets.c').read_text()
    window = (ROOT/'user/libgui/window.c').read_text()
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
typedef struct {int x,y,w,h;} rect_t;
typedef struct {rect_t frame;} window_t;
static int scale;
static int gui_screen_scale(void){return scale;}
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
'''
    for name in ['thumb_rect', 'gui_scrollbar_hit']:
        code += '\n' + function(widget, name)
    code += '\n' + function(window, 'window_content_scale')
    code += r'''
int main(void){
    const int totals[]={1,2,25,1000,1000000,INT_MAX};unsigned tests=0;
    for(unsigned a=0;a<sizeof totals/sizeof totals[0];a++)for(int visible=1;visible<=100;visible+=3)
    for(int height=1;height<2000;height+=17){
        int total=totals[a],span=total-visible;rect_t r={-50,40,12,height};
        for(int i=0;i<5;i++){
            int offset=i==0?INT_MIN:i==1?0:i==2?span/2:i==3?span:INT_MAX;
            rect_t t=thumb_rect(r,offset,visible,total);
            if(total>visible&&height>2){
                assert(t.h>0&&t.h<=height-2&&t.y>=r.y+1&&t.y+t.h<=r.y+height-1);
                int low=gui_scrollbar_hit(r,offset,visible,total,INT_MIN);
                int high=gui_scrollbar_hit(r,offset,visible,total,INT_MAX);
                assert(!low);assert(high==(t.h<height-2?span:0));
                int prev=0;for(int y=r.y;y<=r.y+r.h;y+=7){
                    int n=gui_scrollbar_hit(r,offset,visible,total,y);
                    assert(n>=prev&&n>=0&&n<=span);prev=n;
                }
            }else assert(!t.h&&!gui_scrollbar_hit(r,offset,visible,total,200));
            tests++;
        }
    }
    for(scale=1;scale<=4;scale++)for(int w=200;w<=8000;w+=111)for(int h=100;h<=4500;h+=137){
        window_t win={{0,0,w,h}};assert(window_content_scale(&win)==scale);
    }
    assert(!thumb_rect((rect_t){0,0,1,10},0,1,100).h);
    assert(!gui_scrollbar_hit((rect_t){0,0,12,10},0,-1,INT_MAX,INT_MAX));
    printf("PASS shared UI: %u scrollbar bounds/monotonic cases including INT_MAX lists, narrow tracks, clamped offsets; stable user-selected typography across window sizes\n",tests);
}
'''
    # Both client repaint sites preserve callback re-invalidation. The pump
    # must also damage the whole visual extent for direct needs_paint writers.
    for name in ['repaint_area', 'wm_pump']:
        body=function(window,name)
        at=body.index('w->needs_paint = false')
        assert body.index('win_proc(w, &'+('ev' if name == 'repaint_area' else 'paint')+')',at)>at
    pump=function(window,'wm_pump')
    assert 'wm_damage(wm, window_visual_bounds(w->frame))' in pump
    run_test(code,'ui_controls')


if __name__ == '__main__':
    main()
