#!/usr/bin/env python3
"""Actual display-open control flow; mocked framebuffer/input/GPU boundaries.

The separate backend test executes the real constructor and counts allocations.
This test checks gate/constructor order and failure cleanup, not native boot.
"""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    src = (ROOT / 'user/libgui/display.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t colour_t;
typedef struct {uint64_t address;unsigned width,height,pitch,bpp;} kframebuffer_t;
typedef struct {colour_t *pixels;int width,height,stride;bool owns_pixels;void *gpu;} surface_t;
typedef struct {kframebuffer_t info;surface_t screen,*back;int input_fd;
                bool have_mouse,driver_updates,hardware_cursor;} display_t;
typedef struct {colour_t pixels[64*64];unsigned width,height;int hot_x,hot_y;} kcursor_t;
#define SYS_FRAMEBUFFER 1
#define SYS_MOUSEPOS 2
#define O_RDONLY 0
#define STDERR_FD 2
#define fprintf(...) ((void)0)
#define snprintf(...) ((void)0)
static unsigned step,gate_step,create_step,creates,destroys,opens,validates;
static unsigned bpp=32,capability=2;
static bool gate_ok=true,create_ok=true,input_ok=true,expected_gpu;
static intptr_t fb_error;
static surface_t backing;
static int gpu_marker;
static void log_write(int level,const char *tag,const char *message){(void)level;(void)tag;(void)message;}
static intptr_t syscall6(int op,intptr_t p,int a,int b,int c,int d,int e){
    assert(!a&&!b&&!c&&!d&&!e);
    if(op==SYS_FRAMEBUFFER){
        if(fb_error)return fb_error;
        *(kframebuffer_t *)p=(kframebuffer_t){0x100000,7680,1440,30720,bpp};return 0;
    }
    assert(op==SYS_MOUSEPOS);((int*)p)[0]=100;((int*)p)[1]=200;return 1;
}
static void gui_set_default_scale(int n){assert(n==2);}
static int gui_scale_for_width(int w){assert(w==7680);return 2;}
static int gpu_can_draw(void){return capability;}
static bool gui_gpu_validate(void){validates++;gate_step=++step;return gate_ok;}
static surface_t *surface_create_target(int w,int h,bool gpu){
    creates++;create_step=++step;assert(w==7680&&h==1440&&gpu==expected_gpu);
    if(gpu)assert(gate_ok&&gate_step&&gate_step<create_step);
    if(!create_ok)return NULL;
    backing=(surface_t){.width=w,.height=h,.stride=w,.owns_pixels=true,
                        .gpu=gpu?&gpu_marker:NULL};return &backing;
}
static void surface_destroy(surface_t *s){assert(s==&backing);destroys++;}
static int open(const char *p,int mode){assert(!strcmp(p,"/dev/input")&&mode==O_RDONLY);
    assert(creates&&create_ok);opens++;if(!input_ok){errno=EIO;return -1;}return 17;}
static int fb_update(int x,int y,int w,int h){assert(x==0&&y==0&&w==1&&h==1);return 0;}
static void gui_cursor_image(colour_t *p,unsigned *w,unsigned *h,int *x,int *y){
    p[0]=0;*w=*h=1;*x=*y=0;}
static int fb_set_cursor(kcursor_t *p){assert(p->width==1);return 0;}
'''
    # Kestrel is LP64; the Windows host is LLP64. Adapt pointer carriers only.
    body = function(src, 'display_open_backend')
    c += body.replace('long r =', 'intptr_t r =').replace('(long)', '(intptr_t)')
    c += r'''
static void reset(void){
    step=gate_step=create_step=creates=destroys=opens=validates=0;
    bpp=32;capability=2;gate_ok=create_ok=input_ok=true;expected_gpu=true;fb_error=0;errno=0;
}
int main(void){
    display_t d;reset();assert(display_open_backend(&d,true));
    assert(d.back==&backing&&d.back->gpu&&!d.back->pixels&&d.input_fd==17);
    assert(validates==1&&creates==1&&opens==1&&!destroys);
    assert(d.have_mouse&&d.driver_updates&&d.hardware_cursor);
    reset();gate_ok=false;assert(!display_open_backend(&d,true));
    assert(errno==EIO&&validates==1&&!creates&&!opens&&!d.back);
    reset();create_ok=false;assert(!display_open_backend(&d,true));
    assert(errno==EIO&&validates==1&&creates==1&&!opens&&!destroys&&!d.back);
    reset();input_ok=false;assert(!display_open_backend(&d,true));
    assert(validates==1&&creates==1&&opens==1&&destroys==1&&!d.back);
    reset();expected_gpu=false;assert(display_open_backend(&d,false));
    assert(!validates&&creates==1&&!d.back->gpu);
    reset();expected_gpu=false;capability=0;assert(display_open_backend(&d,true));
    assert(!validates&&creates==1&&!d.back->gpu);
    reset();expected_gpu=false;create_ok=false;assert(!display_open_backend(&d,false));
    assert(errno==ENOMEM&&!validates&&creates==1&&!opens);
    reset();fb_error=-EBUSY;assert(!display_open_backend(&d,true));
    assert(errno==EBUSY&&!validates&&!creates&&!opens);
    reset();fb_error=-EIO;assert(!display_open_backend(&d,true));
    assert(errno==ENODEV&&!validates&&!creates&&!opens);
    reset();bpp=16;assert(!display_open_backend(&d,true));assert(!validates&&!creates&&!opens);
    puts("PASS actual display startup: native validation precedes GPU target allocation; ten success/failure/compatibility paths, cleanup and input/cursor initialization (modeled boundaries)");
}
'''
    run_test(c, 'display_gpu_startup')


if __name__ == '__main__':
    main()
