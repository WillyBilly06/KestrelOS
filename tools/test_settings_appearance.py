#!/usr/bin/env python3
"""Execute the measured Settings appearance layout and scrolling boundaries."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
typedef struct {int x,y,w,h;} rect_t;
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
typedef uint32_t colour_t;
#define RGB(r,g,b) (((r)<<16)|((g)<<8)|(b))
static struct {colour_t desktop_top,desktop_bottom;} g_theme;
'''
    code += (ROOT / 'user/desktop/settings_appearance.h').read_text()
    code += function((ROOT / 'user/desktop/desktop.c').read_text(), 'desktop_wallpaper_colours')
    code += r'''
int main(void){
    unsigned cases=0;
    for(int scale=1;scale<=8;scale++)
    for(int w=1;w<=8000;w+=37)
    for(int count=1;count<=64;count+=3){
        int widths[4]={230*scale,32*scale,190*scale,100*scale};
        int counts[4]={count,count+1,count+2,3};
        rect_t viewport={24,80,w,317};
        settings_appearance_t a=settings_appearance_layout(viewport,scale,16*scale,widths,counts);
        int end=0;
        for(int group=0;group<4;group++){
            settings_grid_t g=a.groups[group];
            assert(g.columns>=1 && g.count==counts[group]);
            assert(a.labels[group]>=end);
            assert(g.bounds.y>a.labels[group]);
            for(int i=0;i<g.count;i++){
                rect_t r=settings_grid_item(g,i,0,0);
                assert(r.x>=0 && r.x+r.w<=viewport.w);
                assert(r.y>=g.bounds.y && r.y+r.h<=g.bounds.y+g.bounds.h);
                if(i){rect_t p=settings_grid_item(g,i-1,0,0);
                    assert(p.y+p.h<=r.y || p.x+p.w<=r.x);}
            }
            assert(!settings_grid_item(g,-1,0,0).w);
            assert(!settings_grid_item(g,g.count,0,0).w);
            end=g.bounds.y+g.bounds.h;
            if(group==2){assert(a.animation_y>=end);assert(a.animation.y>a.animation_y);
                assert(a.animation.y+a.animation.h<=a.labels[3]);}
        }
        assert(a.total_height>=end);
        int bottom=settings_scroll_clamp(INT64_MAX,a.total_height,viewport.h);
        assert(bottom>=0 && a.total_height-bottom<=viewport.h);
        assert(settings_scroll_clamp(INT64_MIN,a.total_height,viewport.h)==0);
        rect_t last=settings_grid_item(a.groups[3],2,viewport.x,viewport.y-bottom);
        assert(last.y+last.h<=viewport.y+viewport.h);
        assert(settings_scroll_clamp(bottom,a.total_height,INT_MAX)==0);
        cases++;
    }
    assert(!settings_grid(0,0,1,1,1,1).count);
    assert(!settings_grid(100,0,0,1,1,1).count);
    assert(!settings_grid(100,0,1,1,1,1025).count);
    assert(!settings_scroll_clamp(10,100,0));
    for(int k=1;k<=8;k++)for(int w=1;w<=2400;w+=7)for(int h=1;h<200;h+=11){
        rect_t r={24,80,w,h};settings_choice_parts_t p=settings_choice_parts(r,k);
        rect_t parts[]={p.preview,p.label,p.marker};
        for(int i=0;i<3;i++){
            rect_t a=parts[i];
            assert(a.x>=r.x&&a.y>=r.y&&a.x+a.w<=r.x+r.w&&a.y+a.h<=r.y+r.h);
            for(int j=0;j<i;j++){
                rect_t b=parts[j];
                if(a.w&&a.h&&b.w&&b.h)
                    assert(a.x+a.w<=b.x||b.x+b.w<=a.x||a.y+a.h<=b.y||b.y+b.h<=a.y);
            }
        }
    }
    colour_t top,bottom;g_theme.desktop_top=0x123456;g_theme.desktop_bottom=0xabcdef;
    desktop_wallpaper_colours(0,&top,&bottom);assert(top==0x123456&&bottom==0xabcdef);
    desktop_wallpaper_colours(1,&top,&bottom);assert(top==0x2a2f38&&bottom==0x14171c);
    desktop_wallpaper_colours(2,&top,&bottom);assert(top==0x0c0e14&&bottom==top);
    desktop_wallpaper_colours(3,&top,&bottom);assert(top==0x0b1e2a&&bottom==0x123a38);
    assert(g_theme.desktop_top==0x123456&&g_theme.desktop_bottom==0xabcdef);
    printf("SETTINGS_APPEARANCE_PASS %u measured/wrapped layout cases\n",cases);
}
'''
    source = (ROOT / 'user/desktop/app_settings.c').read_text()
    assert 'surface_set_clip(s,rect_intersection(saved_clip,nav.body))' in source
    assert 'surface_set_clip(s,saved_clip)' in source
    assert 'if (!rect_contains(nav.body,ev->x,ev->y)) return false;' in source
    assert 'theme_button_rect(st, s, i)' in source
    assert 'appearance_rows(' not in source
    assert 'desktop_wallpaper_colours(group == 0 ? st->wallpaper : index,&top,&bottom)' in source
    assert 'appearance_preview_button(st,s,r,group,i,name,hover,selected)' in source
    run_test(code, 'settings_appearance')


if __name__ == '__main__':
    main()
