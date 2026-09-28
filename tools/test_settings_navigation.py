#!/usr/bin/env python3
"""Execute responsive Settings geometry; prevent native/firmware event crossover."""
from test_gpu_stable_candidate import ROOT, run_test


def main():
    source = (ROOT / 'user/desktop/app_settings.c').read_text()
    assert 'if (!nav.sidebar || ev->x >= nav.sidebar || ev->kind == WE_MOUSE_UP) return changed;' in source
    assert 'settings_paint(st, s, st->pointer_x, st->pointer_y)' in source
    assert 'ev->key == KK_PAGEUP || ev->key == KK_PAGEDOWN' in source
    assert 'sidebar_item_rect(int i)' not in source
    code = r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
typedef struct { int x,y,w,h; } rect_t;
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
''' + (ROOT / 'user/desktop/settings_navigation.h').read_text() + r'''
static void inside(rect_t r,int w,int h){
    assert(r.x>=0 && r.y>=0 && r.w>=0 && r.h>=0);
    assert((int64_t)r.x+r.w<=w && (int64_t)r.y+r.h<=h);
}
int main(void){
    int widths[]={1,2,3,16,100,320,640,780,1024,1920,3840,7680,INT_MAX};
    int heights[]={1,2,3,16,100,240,440,560,1080,2160,4320,INT_MAX};
    unsigned cases=0;
    for(unsigned wi=0;wi<sizeof widths/sizeof widths[0];wi++)
    for(unsigned hi=0;hi<sizeof heights/sizeof heights[0];hi++)
    for(int scale=1;scale<=8;scale++)
    for(int pages=1;pages<=32;pages++){
        int w=widths[wi],h=heights[hi],fh=16*scale,side=190*scale;
        settings_navigation_t n=settings_navigation(w,h,scale,fh,side,pages);
        inside(n.body,w,h);
        inside(n.status,w,h);
        assert(n.body.y+n.body.h<=n.status.y);
        if(n.sidebar){
            assert(n.sidebar==side && !n.top && n.body.w>0);
            assert((int64_t)w>=side+400*scale);
            assert(h>=36*scale+fh+pages*(fh*5/3));
        }else{
            inside(n.previous,w,h);inside(n.next,w,h);inside(n.caption,w,h);
            assert(n.previous.x+n.previous.w<=n.caption.x);
            assert(n.caption.x+n.caption.w<=n.next.x);
            assert(n.body.y>=n.top);
        }
        cases++;
    }
    assert(settings_navigation(780,560,1,16,190,7).sidebar==190);
    assert(settings_navigation(780,560,3,48,570,7).sidebar==0);
    assert(settings_navigation(1920,200,1,16,190,7).sidebar==0);
    assert(settings_navigation(1920,1080,1,16,INT_MAX,7).sidebar==0);
    assert(!settings_navigation(0,560,1,16,190,7).body.w);
    assert(!settings_navigation(780,560,0,16,190,7).body.w);
    assert(!settings_navigation(780,560,1,513,190,7).body.w);
    assert(!settings_navigation(780,560,1,16,-1,7).body.w);
    assert(!settings_navigation(780,560,1,16,190,33).body.w);
    printf("SETTINGS_NAVIGATION_PASS %u geometry cases\n",cases);
    return 0;
}
'''
    run_test(code, 'settings_navigation')


if __name__ == '__main__':
    main()
