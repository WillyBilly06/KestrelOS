"""Exercise production mesh generation and responsive toolbar geometry."""
from test_gpu_stable_candidate import ROOT, run_test, function

def main():
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
typedef struct {float x,y,z,r,g,b,a;} api_vertex_t;
#include "vulkan_mesh.h"
int main(void){
    unsigned total=0;
    for(unsigned s=0;s<6;s++)for(unsigned d=0;d<=3;d++){
        unsigned n=vk_mesh_count(s,d);assert(n&&n%3==0&&n<=55296);
        if(d)assert(n>vk_mesh_count(s,d-1));
        api_vertex_t *p=calloc(n+1,sizeof(*p));assert(p);
        p[n].x=12345;
        assert(!vk_mesh_build(p,n-1,s,d));
        assert(vk_mesh_build(p,n,s,d)&&p[n].x==12345);
        for(unsigned i=0;i<n;i++)for(unsigned k=0;k<7;k++)assert(isfinite(((float*)&p[i])[k]));
        for(unsigned i=0;i<n;i+=3){
            float a[3]={p[i+1].x-p[i].x,p[i+1].y-p[i].y,p[i+1].z-p[i].z};
            float b[3]={p[i+2].x-p[i].x,p[i+2].y-p[i].y,p[i+2].z-p[i].z};
            float c[3]={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
            float x=(p[i].x+p[i+1].x+p[i+2].x)/3,y=(p[i].y+p[i+1].y+p[i+2].y)/3,z=(p[i].z+p[i+1].z+p[i+2].z)/3;
            float dot=c[0]*x+c[1]*y+c[2]*z;
            if(s==1){float r=sqrtf(x*x+y*y);dot=c[0]*x*(1-1.1f/r)+c[1]*y*(1-1.1f/r)+c[2]*z;}
            if(s==2)dot=c[2];
            assert(dot>=-0.000001f); // pole degenerates may round through zero
        }
        total+=n/3;free(p);
    }
    assert(!vk_mesh_count(6,0)&&!vk_mesh_count(0,4));
    printf("PASS Vulkan meshes: 6 shapes x 4 densities, %u triangles; finite/bounded/outward geometry, capacity guards\n",total);
}
'''
    run_test(code,'vulkan-mesh',[ROOT/'user/desktop'])
    src=(ROOT/'user/desktop/app_gl.c').read_text()
    controls=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,w,h;} rect_t;
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static int scale=1;
static int gui_scale(void){return scale;}
#define FONT_UI 0
static int gui_font_height(int f){(void)f;return 16*scale;}
static int gui_text_width(int f,const char *s){(void)f;return (int)strlen(s)*8*scale;}
#define BAR_H (bar_height())
#define BUTTON_COUNT 7
'''
    for name in ('bar_height','slot_widest','button_width','button_rect','toolbar_height'):
        # helper used by most tests excludes const char* signatures
        if name=='slot_widest':
            a=src.index('static const char *slot_widest(');b=src.index('\n}',a)+2
            controls+=src[a:b]+'\n'
        else:controls+=function(src,name)+'\n'
    controls+=r'''
int main(void){for(scale=1;scale<=3;scale++)for(int width=320;width<=2560;width+=13){
    rect_t r[7];for(int i=0;i<7;i++){
        r[i]=button_rect(i,width);assert(r[i].x>=0&&r[i].x+r[i].w<=width&&r[i].w>0);
        assert(r[i].y+r[i].h<=toolbar_height(width));
        for(int j=0;j<i;j++)assert(r[i].x>=r[j].x+r[j].w || r[j].x>=r[i].x+r[i].w ||
            r[i].y>=r[j].y+r[j].h || r[j].y>=r[i].y+r[i].h);
    }
}puts("PASS 3D toolbar: no overlapping/out-of-width controls, 320..2560px, scales 1..3");}
'''
    run_test(controls,'vulkan-toolbar')

if __name__=='__main__':main()
