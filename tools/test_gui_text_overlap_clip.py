#!/usr/bin/env python3
"""Track visible production text boxes, not clipped-away Settings content."""
from test_gpu_stable_candidate import ROOT, function, run_test
src=(ROOT/'user/libgui/draw.c').read_text()
code=r'''
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef struct {int x,y,w,h;} rect_t;
typedef struct {rect_t clip;} surface_t;
static unsigned warnings;
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static rect_t surface_clip(const surface_t*s){return s->clip;}
static rect_t rect_intersection(rect_t a,rect_t b){
    int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r>x?r-x:0,d>y?d-y:0);
}
static void log_write(int level,const char*tag,const char*text){assert(level==2&&!strcmp(tag,"gui")&&text);warnings++;}
'''
code+=src[src.index('#define TEXT_RUNS_TRACKED'):src.index('/* Do these two runs')]
for name in ('boxes_meet','boxes_crowd','note_text_run','gui_text_frame_begin'):
    code+=function(src,name)+'\n'
code+=r'''
int main(void){
    surface_t s={{193,20,500,460}};
    gui_text_frame_begin();
    note_text_run(&s,193,498,300,20,"Startup arrangement",19);
    assert(!runs_used&&!warnings);
    s.clip=rect_make(193,490,500,40);
    note_text_run(&s,205,505,200,20,"No networks found.",18);
    assert(runs_used==1&&!warnings);
    /* Genuine visible overlap is still detected. */
    note_text_run(&s,205,505,200,20,"Actual collision",16);
    assert(warnings==1);
    gui_text_frame_begin();warnings=0;s.clip=rect_make(0,0,100,10);
    note_text_run(&s,0,0,100,30,"Page",4);
    s.clip=rect_make(0,20,100,10);
    note_text_run(&s,0,0,100,30,"Footer",6);
    assert(runs_used==2&&!warnings&&runs[0].h==10&&runs[1].y==20);
    puts("PASS clipped text overlap tracking: hidden page/footer, disjoint clips, genuine collision preserved");
}
'''
run_test(code,'gui_text_overlap_clip')
