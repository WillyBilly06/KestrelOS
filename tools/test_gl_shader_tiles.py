#!/usr/bin/env python3
"""Actual programmable fragment tiler: exact ordered pixel ownership/admission.

The modeled boundary verifies every original bounding-box/command pair occurs
exactly once and in original order, including blended/depth-tested primitives.
It does not claim native GPU timing or execute a CPU rendering fallback.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_gl_gpu import strip_includes

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {int x,y,w,h;} rect_t;
static rect_t rect_make(int x,int y,int w,int h){return (rect_t){x,y,w,h};}
static bool rect_empty(rect_t r){return r.w<=0||r.h<=0;}
static rect_t rect_intersection(rect_t a,rect_t b){
    int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w;
    int d=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r>x?r-x:0,d>y?d-y:0);
}
static rect_t rect_union(rect_t a,rect_t b){
    int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
    int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w;
    int d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r-x,d-y);
}
'''
SETUP = r'''
typedef struct {unsigned lanes;ksh_status_t results[1];} test_vm_t;
static struct {
    kshr_job_t *fragment;
    kshr_command_t pending[KG3D_MAX_COMMANDS];
    test_vm_t vm;
    struct {uint64_t colour,depth;} target;
    rect_t bounds;
    bool active,failed;
    uint64_t batches;
} pipeline;
static struct {uint64_t stat_shader_instructions;} g_gl;
static kshr_command_t original[KG3D_MAX_COMMANDS];
static unsigned count,width,height,launches,fail_at,failures;
static uint64_t *seen,admitted_work;
static bool failure(const char *s){(void)s;failures++;pipeline.failed=true;return false;}
static void log_write(int level,const char *tag,const char *s){(void)level;(void)tag;(void)s;}
static bool gl_gpu_vm_raster(test_vm_t *vm,uint64_t colour,uint64_t depth,const kshr_job_t *job){
    assert(colour==123&&depth==456);
    assert(job->clip_w&&job->clip_h&&job->clip_x+job->clip_w<=width&&job->clip_y+job->clip_h<=height);
    unsigned work=0,previous=0;
    for(unsigned i=0;i<job->command_count;i++){
        unsigned id=(unsigned)job->commands[i].varying[0][0][0];
        assert(id<count&&(!i||id>previous));previous=id;
        assert(!memcmp(&job->commands[i],&original[id],sizeof original[id]));
        work+=(job->commands[i].raster.flags&KG3D_LINE)?KG3D_LINE_WORK:1u;
    }
    uint64_t covered=0;
    for(unsigned i=0;i<job->command_count;i++){
        const kg3d_command_t *c=&job->commands[i].raster;
        rect_t hit=rect_intersection(rect_make(c->x,c->y,c->width,c->height),
            rect_make(job->clip_x,job->clip_y,job->clip_w,job->clip_h));
        covered+=(uint64_t)hit.w*hit.h*((c->flags&KG3D_LINE)?KG3D_LINE_WORK:1u);
    }
    uint64_t cost=covered*job->budget,checks=(uint64_t)job->clip_w*job->clip_h*work;
    if(checks>cost)cost=checks;
    assert(cost<=KSH_MAX_WORK);admitted_work+=cost;
    launches++;if(fail_at==launches)return false;
    for(unsigned y=job->clip_y;y<job->clip_y+job->clip_h;y++)
    for(unsigned x=job->clip_x;x<job->clip_x+job->clip_w;x++){
        uint64_t *mask=&seen[y*width+x];
        for(unsigned i=0;i<job->command_count;i++){
            const kg3d_command_t *c=&job->commands[i].raster;
            if((int)x<c->x||(int)y<c->y||(int)x>=c->x+c->width||(int)y>=c->y+c->height)continue;
            unsigned id=(unsigned)job->commands[i].varying[0][0][0];uint64_t bit=1ull<<id;
            assert(!(*mask&bit)); // no duplicate command/pixel pair
            assert(!(*mask&~(bit-1))); // original order at every affected pixel
            *mask|=bit;
        }
    }
    vm->lanes=1;vm->results[0]=(ksh_status_t){KSH_COMPLETE,1};return true;
}
'''
TEST = r'''
static uint32_t random_state=0x1829;
static unsigned rnd(void){random_state=random_state*1664525u+1013904223u;return random_state;}
static unsigned run(unsigned scenario,unsigned budget,unsigned n,unsigned w,unsigned h){
    count=n;width=w;height=h;launches=admitted_work=0;
    seen=calloc((size_t)width*height,sizeof(*seen));assert(seen);
    kshr_job_t job={.budget=budget,.command_count=count};pipeline.fragment=&job;
    pipeline.active=true;pipeline.failed=false;pipeline.target.colour=123;pipeline.target.depth=456;
    unsigned work=0;
    for(unsigned i=0;i<count;i++){
        rect_t r;
        if(!scenario)r=rect_make(0,0,w,h); // dense: do not inflate original dispatch count
        else if(scenario==1)r=rect_make(i&1?w-8:0,i&2?h-8:0,8,8); // four small corner clusters
        else if(scenario==2)r=rect_make((i%8)*(w/8),(i/8)*(h/8),w/16+1,h/16+1);
        else r=rect_make(rnd()%w,rnd()%h,1+rnd()%w,1+rnd()%h);
        r=rect_intersection(r,rect_make(0,0,w,h));
        kshr_command_t c={.raster={.x=r.x,.y=r.y,.width=r.w,.height=r.h,
            .flags=KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_BLEND|((i%7==0)?KG3D_LINE:0)}};
        c.varying[0][0][0]=(float)i;original[i]=job.commands[i]=c;
        pipeline.bounds=i?rect_union(pipeline.bounds,r):r;
        work+=(c.raster.flags&KG3D_LINE)?KG3D_LINE_WORK:1u;
    }
    uint64_t limit=KSH_MAX_WORK/((uint64_t)work*budget);assert(limit);
    unsigned tw=(unsigned)pipeline.bounds.w;if(tw>limit)tw=(unsigned)limit;
    unsigned th=(unsigned)(limit/tw);
    unsigned old=((unsigned)pipeline.bounds.w+tw-1)/tw*((unsigned)pipeline.bounds.h+th-1)/th;
    bool ok=gl_shader_gpu_flush();
    if(fail_at){assert(!ok&&pipeline.failed&&launches==fail_at);}
    else{
        assert(ok&&!pipeline.failed&&!job.command_count);
        for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
            uint64_t expected=0;
            for(unsigned i=0;i<count;i++){
                const kg3d_command_t *c=&original[i].raster;
                if((int)x>=c->x&&(int)y>=c->y&&(int)x<c->x+c->width&&(int)y<c->y+c->height)expected|=1ull<<i;
            }
            assert(seen[y*w+x]==expected);
        }
        if(!scenario)assert(launches==old);
        if(scenario==1&&budget==4096&&n==64){
            assert(launches<old/10);
            printf("PASS sparse fragment tiles %ux%u: %u -> %u dispatches, exact ordered pixel ownership\n",w,h,old,launches);
        }
    }
    free(seen);return launches;
}
int main(void){
    const unsigned budgets[]={1,7,127,4096},counts[]={1,2,64};
    for(unsigned s=0;s<4;s++)for(unsigned b=0;b<4;b++)for(unsigned n=0;n<3;n++)
        run(s,budgets[b],counts[n],256,192);
    for(unsigned i=0;i<32;i++)run(3,4096,64,17+rnd()%240,17+rnd()%170);
    run(0,4096,64,1,8192);run(0,4096,64,8192,1);
    run(1,4096,64,1024,768);
    fail_at=2;run(1,4096,64,256,192);assert(failures==1);fail_at=0;
    pipeline.active=false;assert(gl_shader_gpu_flush());
    puts("PASS 84 fragment-tiling cases: dense dispatch count preserved, sparse work culled, line weighting, exact per-pixel order/coverage, boundaries and immediate failure stop");
}
'''

if __name__ == '__main__':
    header=HEADER
    for name in ['gpu3d.h','shader_vm.h','shader_raster.h']:
        header+='\n'+strip_includes((ROOT/'include/kestrel'/name).read_text())
    source=(ROOT/'user/libgl/shadergpu.c').read_text()
    source=source[source.index('static unsigned region_work('):source.index('bool gl_shader_gpu_enqueue(')]
    clang=shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    with tempfile.TemporaryDirectory(prefix='kestrel-fragment-tiles-') as tmp:
        path=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe'
        path.write_text(header+SETUP+source+TEST)
        subprocess.run([clang,'-std=c11','-O2','-Wall','-Wextra',str(path),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
