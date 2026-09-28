#!/usr/bin/env python3
"""Compile legacy GL command/clip/raster/texture code with modeled GPU boundaries.

No CPU color/depth target exists in the native cases. This isolates the legacy
CPU command frontend, not the current resident fixed triangle implementation;
test_gl_fixed_gpu.py compiles that implementation against its modeled VM boundary.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import sys
from test_gpu_stable_candidate import function

ROOT = Path(__file__).resolve().parents[1]


def strip_includes(text):
    return re.sub(r"^#include[^\n]*\n", "", text, flags=re.M)


def main():
    header = r'''
#ifndef TEST_GL_GPU_H
#define TEST_GL_GPU_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static inline float radiansf(float degrees) { return degrees * (float)(M_PI / 180.0); }
typedef uint32_t colour_t;
#define RGB(r,g,b) (((r)<<16)|((g)<<8)|(b))
#define RGB_R(c) (((c)>>16)&255)
#define RGB_G(c) (((c)>>8)&255)
#define RGB_B(c) ((c)&255)
struct gui_gpu_surface {unsigned id;};
typedef struct {colour_t *pixels;int width,height,stride;bool owns_pixels;struct gui_gpu_surface *gpu;} surface_t;
typedef struct {int x,y,w,h;} rect_t;
static inline rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
rect_t rect_intersection(rect_t,rect_t);
rect_t rect_union(rect_t,rect_t);
bool rect_empty(rect_t);
rect_t surface_clip(const surface_t *);
bool gui_gpu_flush(void);
bool gui_gpu_cpu_access(surface_t *);
bool gui_gpu_depth_readback(const surface_t *,float *,unsigned);
surface_t *gui_gpu_image(const colour_t *,int,int);
void surface_destroy(surface_t *);
void log_write(int,const char *,const char *);
'''
    for path in ["include/kestrel/gpu3d.h", "include/kestrel/shader_vm.h", "include/kestrel/shader_raster.h",
                 "user/libgl/GL.h", "user/libgl/shader.h", "user/libgl/glstate.h"]:
        header += "\n" + strip_includes((ROOT / path).read_text())
    header += "\nbool gui_gpu_draw3d(surface_t *,const surface_t *,const kg3d_command_t *,unsigned,rect_t);\n#endif\n"
    harness = r'''
#include "test.h"
static unsigned submissions,records,images,destroys,readbacks,demotions;
static bool fail_gpu,program_active;
static kg3d_command_t observed[2048];
static unsigned observed_count;
static sh_program_t program;
static struct gui_gpu_surface native_handle={1};
rect_t rect_intersection(rect_t a,rect_t b) {
    int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r>x?r-x:0,d>y?d-y:0);
}
rect_t rect_union(rect_t a,rect_t b) {
    int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
    int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r-x,d-y);
}
bool rect_empty(rect_t r){return r.w<=0 || r.h<=0;}
rect_t surface_clip(const surface_t *s){return rect_make(0,0,s->width,s->height);}
void log_write(int level,const char *tag,const char *note){(void)level;(void)tag;puts(note);}
bool gui_gpu_flush(void){return !fail_gpu;}
bool gui_gpu_draw3d(surface_t *s,const surface_t *texture,const kg3d_command_t *cmd,unsigned count,rect_t area) {
    assert(s==g_gl.colour && s->gpu && !s->pixels && !g_gl.depth && count && count<=64);
    assert(area.x>=0 && area.y>=0 && area.x+area.w<=s->width && area.y+area.h<=s->height);
    if(fail_gpu)return false;
    if(texture)assert(texture->gpu && !texture->pixels);
    submissions++;records+=count;
    for(unsigned i=0;i<count;i++) {
        assert(observed_count<2048);observed[observed_count++]=cmd[i];
        assert(!(cmd[i].flags&~KG3D_FLAGS));
        if(cmd[i].flags&KG3D_TEXTURE)assert(texture);
        if(cmd[i].flags&(KG3D_CLEAR_DEPTH|KG3D_CLEAR_COLOUR))continue;
        for(unsigned j=0;j<3;j++) {
            assert(isfinite(cmd[i].v[j].x) && fabsf(cmd[i].v[j].x)<=1048576);
            assert(isfinite(cmd[i].v[j].y) && fabsf(cmd[i].v[j].y)<=1048576);
            assert(cmd[i].v[j].inv_w>0 && isfinite(cmd[i].v[j].inv_w));
        }
    }
    return true;
}
surface_t *gui_gpu_image(const colour_t *pixels,int w,int h) {
    assert(pixels && w==2 && h==2);
    surface_t *s=calloc(1,sizeof *s);s->width=w;s->height=h;s->gpu=malloc(sizeof *s->gpu);
    s->gpu->id=++images+2;return s;
}
void surface_destroy(surface_t *s){if(s){assert(s->gpu && !s->pixels);destroys++;free(s->gpu);free(s);}}
bool gui_gpu_depth_readback(const surface_t *s,float *out,unsigned stride) {
    assert(s->gpu && !s->pixels && stride==(unsigned)s->width && !fail_gpu);readbacks++;
    for(int i=0;i<s->width*s->height;i++)out[i]=0.75f;return true;
}
bool gui_gpu_cpu_access(surface_t *s) {
    assert(s->gpu && !fail_gpu);demotions++;s->gpu=NULL;
    s->pixels=calloc((size_t)s->width*s->height,4);return true;
}
bool gl_program_active(void){return program_active;}
bool gl_shader_gpu_active(void){return false;}
bool gl_shader_gpu_flush(void){return true;}
void gl_shader_gpu_release(void){}
/* This older test isolates the legacy raster/texture command frontend. The
 * actual fixed vertex/resident triangle backend is in test_gl_fixed_gpu.py.
 * These reference adapters are test-only, never OS fallback implementations. */
void gl_fixed_capture(float x,float y,float z,float w,gl_vertex_t *v){gl_process_vertex(x,y,z,w,v);}
bool gl_fixed_transform_gpu(gl_vertex_t *v,unsigned n){(void)v;(void)n;return true;}
bool gl_fixed_triangles_gpu(const gl_vertex_t *v,unsigned n){
    assert(!(n%3));
    for(unsigned i=0;i<n;i+=3){
        gl_vertex_t a=v[i],b=v[i+1],c=v[i+2];
        if(g_gl.shade_model==GL_FLAT){
            a.r=b.r=c.r;a.g=b.g=c.g;a.b=b.b=c.b;a.a=b.a=c.a;
        }
        gl_raster_triangle(&a,&b,&c);
    }
    return gl_gpu_flush();
}
void gl_fixed_gpu_release(void){}
void gl_shader_immediate_begin_gpu(GLenum m){(void)m;assert(false);}
void gl_shader_immediate_vertex_gpu(float x,float y,float z,float w){(void)x;(void)y;(void)z;(void)w;assert(false);}
void gl_shader_immediate_end_gpu(void){assert(false);}
bool gl_shader_gpu_enqueue(const kshr_command_t *c){(void)c;assert(false);return false;}
bool gl_shader_draw_gpu(GLenum m,GLint f,GLsizei n,GLenum t,const void *p){(void)m;(void)f;(void)n;(void)t;(void)p;return false;}
const sh_program_t *gl_current_program(void){return program_active?&program:NULL;}
void gl_shader_begin_draw(void){}
void gl_shader_vertex(int index,gl_vertex_t *out){(void)index;memset(out,0,sizeof *out);}
bool gl_shader_fragment(const float varying[SR_VARYING_N][4],float x,float y,float z,float w,float *rgba) {
    (void)varying;(void)x;(void)y;(void)z;(void)w;
    rgba[0]=1;rgba[1]=rgba[2]=0;rgba[3]=1;return true;
}
static void triangle(float z) {
    glBegin(GL_TRIANGLES);
    glColor3f(1,0,0);glVertex3f(-0.75f,-0.5f,z);
    glColor3f(0,1,0);glVertex3f(0.75f,-0.5f,z);
    glColor3f(0,0,1);glVertex3f(0,0.75f,z);glEnd();
}
int main(void) {
    surface_t s={.width=96,.height=64,.stride=96,.owns_pixels=true,.gpu=&native_handle};
    glSetTarget(&s);assert(g_gl.gpu_target && !g_gl.depth && !s.pixels && !demotions);
    glViewport(0,0,96,64);glClearColor(0.1f,0.2f,0.3f,1);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    assert(records==1 && observed[0].flags==96 && observed[0].v[0].z==1);
    glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDepthFunc(GL_LEQUAL);
    triangle(0);assert(observed_count==2);
    kg3d_command_t c=observed[1];
    assert(c.flags==3 && c.depth_func==KG3D_LEQUAL);
    assert(c.v[0].x==12 && c.v[0].y==48 && c.v[0].z==0.5f);
    assert(c.v[1].x==84 && c.v[1].y==48 && c.v[2].x==48 && c.v[2].y==8);
    assert(!g_gl.depth && !s.pixels && !demotions && !readbacks);
    unsigned before=submissions;
    glBegin(GL_TRIANGLES);
    for(unsigned i=0;i<130;i++){glVertex3f(-.5f,-.5f,0);glVertex3f(.5f,-.5f,0);glVertex3f(0,.5f,0);}
    glEnd();assert(submissions-before==3); /* 64+64+2 records, not a fence per triangle */
    before=records;glEnable(GL_CULL_FACE);glCullFace(GL_FRONT_AND_BACK);triangle(0);assert(records==before);
    glDisable(GL_CULL_FACE);glEnable(GL_SCISSOR_TEST);glScissor(20,17,9,11);triangle(0);
    c=observed[observed_count-1];assert(c.x==20 && c.y==17 && c.width==9 && c.height==11);
    glDisable(GL_SCISSOR_TEST);
    /* Clip against every plane, including the eye, without unbounded GPU coords. */
    before=records;
    glBegin(GL_TRIANGLES);glVertex4f(-2,-1,0,-0.5f);glVertex4f(2,-1,0,1);glVertex4f(0,2,0,1);glEnd();
    assert(records>before);
    before=records;triangle(2);assert(records==before); /* GL far plane */
    gl_set_clip_depth_zero_to_one(true);triangle(-.5f);assert(records==before);gl_set_clip_depth_zero_to_one(false);
    glViewport(INT32_MAX,INT32_MAX,INT32_MAX,INT32_MAX);triangle(0);assert(records==before);
    glViewport(0,0,96,64);
    /* Every fixed-function primitive stays on the native target, including
     * line/point draws with texture state enabled (legacy semantics: untextured). */
    glEnable(GL_TEXTURE_2D);before=records;
    glBegin(GL_POINTS);glVertex3f(0,0,0);glVertex3f(2,0,0);glEnd();
    assert(records==before+1);c=observed[observed_count-1];
    assert(c.flags==(KG3D_POINT|KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE));
    assert(c.v[0].x==48 && c.v[0].y==32 && !images);
    before=records;glBegin(GL_LINES);
    glVertex3f(-2,0,0);glVertex3f(2,0,0);glEnd();
    assert(records==before+1);c=observed[observed_count-1];
    assert(c.flags==(KG3D_LINE|KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE));
    assert(c.v[0].x==0 && c.v[1].x==96 && c.depth_bias==0 && !images);
    before=records;glBegin(GL_LINES);
    glVertex3f(2,0,0);glVertex3f(-2,0,0);glEnd();
    assert(records==before+1);c=observed[observed_count-1];
    assert(c.v[0].x==96 && c.v[1].x==0);
    before=records;glBegin(GL_LINES);
    glVertex3f(0,0,0);glVertex3f(0,0,0);glEnd();assert(records==before+1);
    before=records;glBegin(GL_LINE_STRIP);
    glVertex3f(-.5f,0,0);glVertex3f(0,.5f,0);glVertex3f(.5f,0,0);glEnd();
    assert(records==before+2);
    before=records;glBegin(GL_LINE_LOOP);
    glVertex3f(-.5f,0,0);glVertex3f(0,.5f,0);glVertex3f(.5f,0,0);glEnd();
    assert(records==before+3);
    before=records;glBegin(GL_LINES);
    glVertex3f(0,0,2);glVertex3f(.5f,0,2);glEnd();assert(records==before);
    glBegin(GL_LINES);glVertex4f(-.5f,0,0,-.5f);glVertex4f(.5f,0,0,1);glEnd();
    assert(records==before+1);
    glDisable(GL_TEXTURE_2D);
    assert(g_gl.gpu_target && !s.pixels && !g_gl.depth && !demotions && !readbacks);
    unsigned ids[6];glGenTextures(6,ids);glEnable(GL_TEXTURE_2D);
    unsigned char texels[16]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    for(unsigned i=0;i<6;i++) {
        glBindTexture(GL_TEXTURE_2D,ids[i]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,texels);
        triangle(0);
    }
    assert(images==6 && destroys==2); /* four resident images, bounded LRU */
    unsigned resident_images=images;triangle(0);assert(images==resident_images);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,texels);
    assert(destroys==3);triangle(0);assert(images==7);
    glDeleteTextures(6,ids);assert(images==destroys);glDisable(GL_TEXTURE_2D);
    assert(strstr((const char *)glGetString(GL_RENDERER),"GPU"));
    assert(!s.pixels && !g_gl.depth && glGetError()==GL_NO_ERROR);
    /* Non-finite app input must not poison the shared GUI GPU transaction. */
    before=records;glColor4f(NAN,0,0,1);glBegin(GL_TRIANGLES);
    glVertex3f(-.5f,-.5f,0);glVertex3f(.5f,-.5f,0);glVertex3f(0,.5f,0);glEnd();
    assert(records==before && glGetError()==GL_INVALID_VALUE);
    assert(gl_gpu_software("explicit CPU compatibility request"));triangle(0);
    assert(demotions==1 && readbacks==1 && s.pixels && g_gl.depth && !g_gl.gpu_target);
    assert(g_gl.depth[0]==0.75f && g_gl.stat_fragments>0); /* preserve depth before CPU path */
    free(s.pixels);free(g_gl.depth);g_gl.depth=NULL;g_gl.depth_w=g_gl.depth_h=0;
    s.pixels=NULL;s.gpu=&native_handle;program_active=false;glSetTarget(&s);
    fail_gpu=true;triangle(0);assert(glGetError()==GL_OUT_OF_MEMORY);
    assert(!gl_gpu_software("explicit CPU compatibility request after failure"));assert(demotions==1 && s.gpu && !s.pixels && !g_gl.depth);
    assert(glGetError()==GL_OUT_OF_MEMORY);
    printf("PASS legacy GL command frontend: %u records, clipping/state/texture LRU, explicit preserved-depth compatibility and failure gates; fixed triangle setup is a reference adapter, NOT resident backend coverage\n",records);
}
'''
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-gl-gpu-") as tmp:
        tmp = Path(tmp)
        (tmp / "test.h").write_text(header)
        files = []
        for name in ["gl.c", "glmath.c", "gltex.c", "raster.c", "gpu.c"]:
            path = tmp / name
            path.write_text('#include "test.h"\n' + strip_includes((ROOT / "user/libgl" / name).read_text()))
            files.append(str(path))
        (tmp / "test.c").write_text(harness + "\n" +
            function((ROOT / "user/libgl/shaderapi.c").read_text(), "glActiveTexture"))
        exe = tmp / "test.exe"
        subprocess.run([clang, "-std=c11", "-O1", "-Wall", "-Wextra",
                        *files, str(tmp / "test.c"), "-o", str(exe)], check=True)
        if '--compile-only' in sys.argv[1:]:
            print('COMPILED ONLY legacy GL frontend model: executable was not run')
        else:
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
