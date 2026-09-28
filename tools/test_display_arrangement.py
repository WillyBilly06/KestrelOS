#!/usr/bin/env python3
"""Validate real signed-position/orientation proposal geometry before modesets."""
from test_gpu_stable_candidate import ROOT, run_test


def main():
    code = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
'''
    code += '#include "'+(ROOT/'include/kestrel/display_layout.h').as_posix()+'"\n'
    code += '\n'.join(l for l in (ROOT/'kernel/display_layout.c').read_text().splitlines() if not l.startswith('#include'))
    code += r'''
static void reject(dl_placement_t *p,int n,int primary,dl_mode_t mode,int mw,int mh){
    dl_layout_t old,out;memset(&old,0xa5,sizeof old);out=old;
    assert(!display_layout_arrange(p,n,primary,mode,mw,mh,&out));assert(!memcmp(&out,&old,sizeof out));
}
int main(void){
    dl_placement_t p[8];dl_layout_t out;
    unsigned cases=0;
    for(int n=1;n<=8;n++)for(int primary=0;primary<n;primary++)
    for(int rotation=0;rotation<4;rotation++)for(int axis=0;axis<2;axis++){
        int x=-8000,y=-16000,totalw=0,totalh=0;
        for(int i=0;i<n;i++){
            int w=201+71*i,h=123+37*i,r=(rotation+i)%4*90;
            int lw=r==90||r==270?h:w,lh=r==90||r==270?w:h;
            p[i]=(dl_placement_t){x,y,w,h,r,1};
            if(axis){y+=lh;totalh+=lh;if(lw>totalw)totalw=lw;}
            else{x+=lw;totalw+=lw;if(lh>totalh)totalh=lh;}
        }
        assert(display_layout_arrange(p,n,primary,DL_EXTEND,32768,32768,&out));
        assert(out.fb_width==totalw&&out.fb_height==totalh&&out.origin_x==-8000&&out.origin_y==-16000);
        for(int i=0;i<n;i++){
            assert(out.out[i].src_x+out.origin_x==p[i].x&&out.out[i].src_y+out.origin_y==p[i].y);
            assert(out.out[i].out_w==p[i].width&&out.out[i].out_h==p[i].height);
            assert(out.out[i].rotation==p[i].rotation&&out.out[i].active);
            assert(out.out[i].src_x>=0&&out.out[i].src_y>=0);
            assert(out.out[i].src_x+out.out[i].src_w<=out.fb_width&&out.out[i].src_y+out.out[i].src_h<=out.fb_height);
        }
        reject(p,n,primary,DL_EXTEND,totalw-1,totalh);
        reject(p,n,primary,DL_EXTEND,totalw,totalh-1);
        assert(display_layout_arrange(p,n,primary,DL_MIRROR,32768,32768,&out));
        int pr=p[primary].rotation,lw=(pr==90||pr==270)?p[primary].height:p[primary].width;
        int lh=(pr==90||pr==270)?p[primary].width:p[primary].height;
        assert(out.fb_width==lw&&out.fb_height==lh&&!out.origin_x&&!out.origin_y);
        for(int i=0;i<n;i++)assert(!out.out[i].src_x&&!out.out[i].src_y&&out.out[i].src_w==lw&&out.out[i].src_h==lh);
        for(int only=0;only<n;only++){
            assert(display_layout_arrange(p,n,primary,DL_ONLY_OUTPUT(only),32768,32768,&out));
            for(int i=0;i<n;i++)assert(out.out[i].active==(i==only));
            assert(out.fb_width==out.out[only].src_w&&out.fb_height==out.out[only].src_h);
            cases++;
        }
        p[primary].active=0;reject(p,n,primary,DL_EXTEND,32768,32768);
        assert(display_layout_arrange(p,n,primary,DL_ONLY_OUTPUT(primary),32768,32768,&out));
        if(n>1){
            int other=primary?0:1;p[primary].active=1;
            p[other].x=p[primary].x;p[other].y=p[primary].y;
            reject(p,n,primary,DL_EXTEND,32768,32768);
        }
        cases++;
    }
    p[0]=(dl_placement_t){INT_MIN,INT_MIN,32768,32768,270,1};
    assert(display_layout_arrange(p,1,0,DL_EXTEND,32768,32768,&out));
    assert(out.origin_x==INT_MIN&&out.origin_y==INT_MIN);
    p[0].x=p[0].y=INT_MAX;
    assert(display_layout_arrange(p,1,0,DL_EXTEND,32768,32768,&out));
    assert(out.origin_x==INT_MAX&&out.origin_y==INT_MAX&&out.fb_width==32768);
    p[1]=p[0];p[1].x=p[1].y=INT_MIN;reject(p,2,0,DL_EXTEND,INT_MAX,INT_MAX);
    reject(p,1,0,(dl_mode_t)15,32768,32768);reject(p,1,0,DL_ONLY_OUTPUT(1),32768,32768);
    reject(p,1,1,DL_EXTEND,32768,32768);reject(p,0,0,DL_EXTEND,32768,32768);
    reject(p,9,0,DL_EXTEND,32768,32768);reject(NULL,1,0,DL_EXTEND,32768,32768);
    for(int field=0;field<4;field++){
        p[0]=(dl_placement_t){0,0,1920,1080,0,1};
        if(field==0)p[0].width=0;if(field==1)p[0].height=32769;
        if(field==2)p[0].rotation=45;if(field==3)p[0].active=2;
        reject(p,1,0,DL_EXTEND,32768,32768);
    }
    printf("PASS %u signed arrangement/rotation/group combinations; exact extents, independent primary, overlap rejection, bounds/overflow and unchanged rejected output\n",cases);
}
'''
    run_test(code,'display_arrangement')


if __name__ == '__main__':
    main()
