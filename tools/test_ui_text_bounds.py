#!/usr/bin/env python3
"""Execute actual text-fit routines with variable advances and glyph overhang."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    source = (ROOT/'user/libgui/draw.c').read_text()
    code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef int font_id;
typedef unsigned colour_t;
typedef struct {int x,y,w,h;} rect_t;
typedef struct {int unused;} surface_t;
typedef struct {int widths[256],draw_scale;} font_desc_t;
static font_desc_t face;
static rect_t clip, allowed;
static unsigned drawn,cases;
static int advance_sum, max_run;
static char recorded[4096];
static size_t recorded_len;
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static rect_t rect_intersection(rect_t a,rect_t b){
    int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int x2=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w;
    int y2=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,x2>x?x2-x:0,y2>y?y2-y:0);
}
static rect_t surface_clip(surface_t*s){(void)s;return clip;}
static void surface_set_clip(surface_t*s,rect_t r){(void)s;clip=r;}
static font_desc_t font_of(font_id f){(void)f;return face;}
static int gui_font_height(font_id f){(void)f;return 16*face.draw_scale;}
static int gui_font_advance(font_id f,unsigned char ch){(void)f;return face.widths[ch]*face.draw_scale;}
static int gui_text_width(font_id f,const char*t){int w=0;if(t)while(*t)w+=gui_font_advance(f,(unsigned char)*t++);return w;}
static void gui_text_n(surface_t*s,font_id f,int x,int y,const char*t,size_t n,colour_t c){
    (void)s;(void)c;int start=x;
    for(size_t i=0;i<n&&t[i];i++){
        int advance=gui_font_advance(f,(unsigned char)t[i]);
        /* Ink extends past its advance, as some real proportional glyphs do. */
        for(int yy=y;yy<y+gui_font_height(f);yy++)for(int xx=x;xx<x+advance+2;xx++)
            if(xx>=clip.x&&xx<clip.x+clip.w&&yy>=clip.y&&yy<clip.y+clip.h){
                assert(xx>=allowed.x&&xx<allowed.x+allowed.w);
                assert(yy>=allowed.y&&yy<allowed.y+allowed.h);drawn++;
            }
        x+=advance;
        if(recorded_len+1<sizeof recorded)recorded[recorded_len++]=t[i];
    }
    if(x-start>max_run)max_run=x-start;
    advance_sum+=x-start;
}
static void gui_text(surface_t*s,font_id f,int x,int y,const char*t,colour_t c){if(t)gui_text_n(s,f,x,y,t,strlen(t),c);}
'''
    for name in ['gui_text_clipped','gui_text_centred','gui_text_right','gui_text_wrapped']:
        code += '\n'+function(source,name)
    code += r'''
static void begin(rect_t bounds){
    clip=(rect_t){0,0,1000,1000};allowed=rect_intersection(clip,bounds);
    drawn=0;advance_sum=max_run=0;recorded_len=0;memset(recorded,0,sizeof recorded);
}
static void restored(void){assert(clip.x==0&&clip.y==0&&clip.w==1000&&clip.h==1000);cases++;}
int main(void){
    const char *samples[]={"", "WWW", "i.i.i", "WiWi WiWi", "a_very_long_word_that_must_wrap",
                           "one two three\nfour\n\nfive", "    words   spaced    out   ", "..."};
    for(int i=0;i<256;i++)face.widths[i]=3+i%9;
    face.widths['.']=3;
    surface_t s={0};
    for(face.draw_scale=1;face.draw_scale<=4;face.draw_scale++)
    for(int width=-2;width<=181;width+=3)
    for(unsigned sample=0;sample<sizeof samples/sizeof samples[0];sample++){
        rect_t box={30,40,width,77};
        begin(rect_make(30,40,width,gui_font_height(0)));
        gui_text_clipped(&s,0,30,40,width,samples[sample],0);
        assert(advance_sum<=(width>0?width:0));restored();
        begin(box);gui_text_centred(&s,0,box,samples[sample],0);restored();
        begin(box);gui_text_right(&s,0,box,samples[sample],0);restored();
        begin(box);int end=gui_text_wrapped(&s,0,box,samples[sample],0);
        assert(end>=box.y&&end<=box.y+box.h);
        /* A too-wide individual glyph still consumes exactly one character;
         * otherwise every emitted line fits the width. */
        assert(max_run<=(width>0?(width>44?width:44):0));restored();
    }
    face.draw_scale=3;
    begin((rect_t){30,40,3,48});gui_text_clipped(&s,0,30,40,3,"overlong",0);
    assert(!drawn&&!recorded_len);restored();
    begin((rect_t){30,40,18,48});gui_text_clipped(&s,0,30,40,18,"overlong",0);
    assert(!strcmp(recorded,".."));restored();
    face.draw_scale=1;
    begin((rect_t){30,40,20,500});gui_text_wrapped(&s,0,(rect_t){30,40,20,500},"abcdefghijk",0);
    assert(!strcmp(recorded,"abcdefghijk"));restored();
    begin((rect_t){30,40,100,100});gui_text_clipped(&s,0,30,40,100,NULL,0);restored();
    gui_text_wrapped(&s,0,(rect_t){30,40,100,100},NULL,0);restored();
    printf("UI_TEXT_BOUNDS_PASS %u fit/wrap/alignment cases, scaled advances, glyph overhang and restored clips\n",cases);
}
'''
    run_test(code,'ui_text_bounds')


if __name__ == '__main__':
    main()
