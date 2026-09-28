#!/usr/bin/env python3
"""Run the actual application's frame accounting; no GPU performance claim."""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / 'user/desktop/app_gl.c').read_text()
    # Use the production enums/state/name tables, with only opaque dependencies.
    declarations = src[src.index('typedef enum'):src.index('/* -------------------------------------------------------------- the frame */')]
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct gl_scene gl_scene_t;
typedef struct surface surface_t;
static uint64_t clock_us;
static unsigned logs;
static unsigned errors;
static char last_log[256];
static uint64_t uptime_us(void) {return clock_us;}
static void log_write(int level,const char *tag,const char *note) {
    if(level==3){assert(!strcmp(tag,"3d-gpu"));errors++;return;}
    assert(level==1 && !strcmp(tag,"3d-perf"));
    assert(strlen(note)<192);memcpy(last_log,note,strlen(note)+1);logs++;
}
'''
    c += declarations + '\n' + function(src, 'frame_finished')
    c += r'''
typedef unsigned GLenum;
#define GL_NO_ERROR 0u
static bool alloc_failed,api_ok=true,scene_ok=true;
static unsigned allocations,releases,draw_calls,scene_calls,destroyed[4],pending_error;
static void *alloc_state(size_t n,size_t size){
    if(alloc_failed)return NULL;
    void *p=calloc(n,size);assert(p);allocations++;return p;
}
static void free_state(void *p){if(p){free(p);releases++;}}
#define calloc alloc_state
#define free free_state
size_t api_state_size(void){return 32;}
void api_destroy_vulkan(void *p){assert(p);destroyed[API_VULKAN]++;}
void api_destroy_d3d9(void *p){assert(p);destroyed[API_D3D9]++;}
void api_destroy_d3d11(void *p){assert(p);destroyed[API_D3D11]++;}
#define DRAW_STUB(name) bool name(void *p,surface_t *s,int top,int w,int h,float spin,float pitch){\
    (void)p;(void)s;(void)top;(void)w;(void)h;(void)spin;(void)pitch;draw_calls++;clock_us+=125;return api_ok;}
DRAW_STUB(api_draw_vulkan)
DRAW_STUB(api_draw_d3d9)
DRAW_STUB(api_draw_d3d11)
bool api_config_vulkan(void *p,unsigned scene,unsigned detail){assert(p);return scene<6&&detail<4;}
static float radiansf(float x){return x;}
static float degreesf(float x){return x;}
static void glUseProgram(unsigned p){assert(!p);}
static void glGetStats(unsigned *tri,unsigned *frag){*tri=12;*frag=0;}
static GLenum glGetError(void){unsigned e=pending_error;pending_error=0;return e;}
static bool gl_scene_render(gl_scene_t **r,surface_t *s,int top,int w,int h,
                            unsigned scene,float yaw,float pitch,bool lit,bool textured){
    (void)r;(void)s;(void)top;(void)w;(void)h;(void)scene;(void)yaw;(void)pitch;
    (void)lit;(void)textured;scene_calls++;clock_us+=75;return scene_ok;
}
'''
    c += function(src, 'api_release') + '\n' + function(src, 'render')
    c += r'''
static void sample(glapp_t *a,uint64_t start,uint64_t duration,int w,int h) {
    clock_us=start+duration;frame_finished(a,start,w,h);
}
int main(void) {
    glapp_t a={.second_started=1000};
    sample(&a,1000,125,640,480);
    assert(a.frame_us==125 && a.perf_frames==1 && !logs);
    assert(a.perf_total_us==125 && a.perf_peak_us==125);
    sample(&a,2001000-375,375,640,480);
    assert(logs==1 && a.fps==1); // two frames in two seconds, not two FPS
    assert(strstr(last_log,"OpenGL/Cube 640x480 flags=0: 2 frames avg=250 us peak=375 us"));
    assert(strstr(last_log,"excludes composition/present"));
    sample(&a,2002000,50,640,480);
    assert(logs==1 && a.perf_frames==1 && a.perf_peak_us==50);
    a.scene=SCENE_TORUS;
    sample(&a,2003000,70,640,480);
    assert(a.perf_frames==1 && a.perf_total_us==70 && a.perf_scene==SCENE_TORUS);
    a.lit=true;
    sample(&a,2004000,80,640,480);
    assert(a.perf_frames==1 && a.perf_flags==1 && a.perf_total_us==80);
    a.textured=true;
    sample(&a,2005000,90,640,480);
    assert(a.perf_frames==1 && a.perf_flags==3 && a.perf_total_us==90);
    sample(&a,2006000,100,1280,720);
    assert(a.perf_frames==1 && a.perf_total_us==100 && a.perf_w==1280);
    a.api=API_VULKAN;
    sample(&a,2007000,120,1280,720);
    assert(a.perf_frames==1 && a.perf_scene==SCENE_TORUS && !a.perf_flags);
    sample(&a,4007000,140,1280,720);
    assert(logs==2 && strstr(last_log,"Vulkan/Torus 1280x720 flags=0: 2 frames avg=130 us peak=140 us"));
    // Lifetime beyond the 32-bit microsecond boundary, plus a very slow frame.
    memset(&a,0,sizeof a);a.second_started=UINT64_C(5000000000);
    sample(&a,UINT64_C(5000000000),UINT64_C(5000000000),800,600);
    assert(a.frame_us==UINT64_C(5000000000) && logs==3 && a.fps==0);
    assert(strstr(last_log,"avg=5000000000 us peak=5000000000 us"));
    // Millisecond display retains all microsecond precision, including leading zeros.
    char line[64];uint64_t value=1005;
    snprintf(line,sizeof line,"%llu.%03llu ms/frame",(unsigned long long)(value/1000),
             (unsigned long long)(value%1000));
    assert(!strcmp(line,"1.005 ms/frame"));
    // Execute the actual application render path, with only drawing/API edges mocked.
    memset(&a,0,sizeof a);clock_us=0;a.api=API_VULKAN;a.spinning=true;
    render(&a,NULL,0,640,480);
    assert(a.frame_us==125&&a.frames_this_second==1&&a.perf_frames==1&&!a.render_failed);
    a.api=API_D3D9;render(&a,NULL,0,640,480);
    assert(destroyed[API_VULKAN]==1&&a.api_state_for==API_D3D9);
    a.api=API_GL;render(&a,NULL,0,640,480);
    assert(destroyed[API_D3D9]==1&&!a.api_state&&scene_calls==1&&a.frame_us==75);
    for(unsigned api=API_VULKAN;api<=API_D3D11;api++)for(unsigned failure=0;failure<3;failure++){
        memset(&a,0,sizeof a);a.api=api;a.spinning=true;unsigned before=errors;
        alloc_failed=failure==0;api_ok=failure!=1;pending_error=failure==2?0x502:0;
        render(&a,NULL,0,640,480);
        assert(a.render_failed&&!a.spinning&&errors==before+1);
        assert(!a.frames_this_second&&!a.perf_frames);
        unsigned calls=draw_calls;render(&a,NULL,0,640,480);assert(draw_calls==calls);
        api_release(&a);alloc_failed=false;api_ok=true;pending_error=0;
    }
    // Switching releases the previous API even if the viewport is temporarily tiny.
    memset(&a,0,sizeof a);a.api=API_D3D11;render(&a,NULL,0,640,480);
    a.api=API_GL;render(&a,NULL,0,0,0);assert(!a.api_state);
    scene_ok=false;memset(&a,0,sizeof a);a.spinning=true;
    render(&a,NULL,0,640,480);assert(a.render_failed&&!a.spinning&&!a.perf_frames);
    assert(allocations==releases);
    puts("GL_FRAME_TIMING_PASS: sub-ms, elapsed FPS, aggregates, mode resets, 64-bit intervals; actual API switching, failed-frame exclusion and balanced state lifetime");
}
'''
    render = function(src, 'render')
    failure = render[render.index('if (!gl_scene_render'):render.index('glGetStats', render.index('if (!gl_scene_render'))]
    assert 'return;' in failure and 'frame_finished' not in failure
    assert render.count('frame_finished(a,started,w,h)') == 2
    assert 'a->second_started = uptime_us()' in src
    run_test(c, 'gl-frame-timing')


if __name__ == '__main__':
    main()
