"""Compare production batched graph fills against the old pixel-loop oracle."""
from test_gpu_stable_candidate import ROOT, function, run_test

source=(ROOT/'user/desktop/app_taskmgr.c').read_text()
draw=function(source,'draw_graph')
reference=draw.replace('draw_graph(', 'reference_graph(').replace(
    'gui_vline(s,x,mid,r.y+r.h-1-mid,colour_mix(g_theme.field,c,40));',
    'for(int yy=mid;yy<r.y+r.h-1;yy++)gui_pixel(s,x,yy,colour_mix(g_theme.field,c,40));')
assert reference!=draw and 'for(int yy=mid' in reference
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HISTORY 120
typedef uint32_t colour_t;
typedef struct {int x,y,w,h;} rect_t;
typedef struct {uint32_t p[512*256];} surface_t;
typedef struct {float v[HISTORY];int count;float peak;bool peak_fixed;} series_t;
static struct {colour_t field,field_border;} g_theme={0xff222222,0xff555555};
static unsigned calls, pixels;
static colour_t colour_mix(colour_t a,colour_t b,int t){return a^b^(unsigned)t;}
static void dot(surface_t*s,int x,int y,colour_t c){assert(x>=0&&x<512&&y>=0&&y<256);s->p[y*512+x]=c;}
static void gui_pixel(surface_t*s,int x,int y,colour_t c){calls++;pixels++;dot(s,x,y,c);}
static void gui_fill(surface_t*s,rect_t r,colour_t c){calls++;for(int y=0;y<r.h;y++)for(int x=0;x<r.w;x++)dot(s,r.x+x,r.y+y,c);}
static void gui_frame(surface_t*s,rect_t r,colour_t c){(void)s;(void)r;(void)c;calls++;}
static void gui_hline(surface_t*s,int x,int y,int w,colour_t c){calls++;for(int i=0;i<w;i++)dot(s,x+i,y,c);}
static void gui_vline(surface_t*s,int x,int y,int h,colour_t c){calls++;for(int i=0;i<h;i++)dot(s,x,y+i,c);}
static void gui_line(surface_t*s,int x,int y,int xx,int yy,colour_t c){calls++;dot(s,x,y,c);dot(s,xx,yy,c);}
'''
code+=draw+reference+r'''
int main(void){
    static surface_t a,b;series_t g={.peak=1};unsigned cases=0;
    for(int count=0;count<=120;count+=5)for(int height=8;height<=150;height+=7){
        g.count=count;for(int i=0;i<count;i++)g.v[i]=(float)((i*19)%137)/100;
        rect_t r={4,4,300,height};memset(&a,0,sizeof a);memset(&b,0,sizeof b);
        calls=pixels=0;draw_graph(&a,r,&g,0xffabcdef,true);unsigned batched=calls;
        assert(!pixels);calls=0;reference_graph(&b,r,&g,0xffabcdef,true);
        assert(!memcmp(&a,&b,sizeof a));assert(batched<=calls);cases++;
    }
    g.count=120;for(int i=0;i<120;i++)g.v[i]=0.75f;
    calls=0;draw_graph(&a,(rect_t){4,4,300,150},&g,0xffabcdef,true);unsigned batched=calls;
    calls=0;reference_graph(&b,(rect_t){4,4,300,150},&g,0xffabcdef,true);
    printf("Task Manager graph PASS: %u exact pixel comparisons; high-load graph %u -> %u drawing calls\n",cases,calls,batched);
    assert(batched*20<calls);
}
'''
if __name__=='__main__':run_test(code,'taskmgr-graph')
