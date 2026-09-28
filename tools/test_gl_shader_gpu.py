#!/usr/bin/env python3
"""Actual compiler/API/vertex assembly/clip/transport; modeled kernel boundary.

Host VM is used ONLY inside the modeled GPU syscall. Application CPU shader,
pixel/depth fallback, texture readback and fixed raster calls abort the test.
GPU arithmetic/fragment pixels are independently covered by CUDA comparisons.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from test_gl_gpu import strip_includes
ROOT=Path(__file__).resolve().parents[1]

HEADER=r'''
#ifndef TEST_NATIVE_PROGRAM_H
#define TEST_NATIVE_PROGRAM_H
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#define M_PI 3.14159265358979323846
#define SYS_GPU 99
#define GPUOP_SURFACE 8
static inline float radiansf(float x){return x*(float)(M_PI/180);}
static inline size_t strlcpy(char *d,const char *s,size_t n){size_t len=strlen(s);if(n){size_t k=len<n?len:n-1;memcpy(d,s,k);d[k]=0;}return len;}
typedef uint32_t colour_t;
#define RGB(r,g,b) (((r)<<16)|((g)<<8)|(b))
#define RGB_R(c) (((c)>>16)&255)
#define RGB_G(c) (((c)>>8)&255)
#define RGB_B(c) ((c)&255)
struct gui_gpu_surface {unsigned id;bool invalid;};
typedef struct {colour_t *pixels;int width,height,stride;bool owns_pixels;struct gui_gpu_surface *gpu;} surface_t;
typedef struct {int x,y,w,h;} rect_t;
typedef struct {uint64_t colour,depth;unsigned pitch,depth_pitch;} gui_gpu_target_t;
static inline rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
rect_t rect_intersection(rect_t,rect_t);rect_t rect_union(rect_t,rect_t);
bool rect_empty(rect_t);rect_t surface_clip(const surface_t *);
bool gui_gpu_flush(void);bool gui_gpu_cpu_access(surface_t *);
bool gui_gpu_depth_readback(const surface_t *,float *,unsigned);
surface_t *gui_gpu_image(const colour_t *,int,int);void surface_destroy(surface_t *);
bool gui_gpu_target(surface_t *,bool,gui_gpu_target_t *);void gui_gpu_invalidate(surface_t *);
void log_write(int,const char *,const char *);
intptr_t syscall6(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t);
void *test_texture_alloc(size_t);
void *test_staging_alloc(size_t);
void test_vm_transfer_role(bool);
static inline void sleep_ms(uint64_t ms){assert(ms==1);} // modeled scheduler yield, no host sleep
'''
HARNESS=r'''
#include "test.h"
typedef struct {uint64_t handle,bytes;unsigned char *data;} memory;
static memory pool[32];static uint64_t next_handle=100;
static unsigned creates,destroys,requests,vertex_launches,raster_launches,texture_uploads,invalidations;
static unsigned records,vertex_lanes;
static unsigned geometry_launches,vertex_readbacks;
static kshr_command_t observed[20000];
static bool fail_vertex,fail_raster,fail_texture,bad_status,in_model;
static bool fail_texture_alloc;
static bool fail_geometry;
int test_model_setup(const kshs_submission_t*,const float*,const ksh_status_t*,void*,kshr_command_t*,unsigned,unsigned*);
static unsigned staging_fail_at,staging_allocations;
static bool transfer_texture;
void test_vm_transfer_role(bool texture){transfer_texture=texture;}
void *test_texture_alloc(size_t size){return fail_texture_alloc?NULL:malloc(size);}
void *test_staging_alloc(size_t size){return ++staging_allocations==staging_fail_at?NULL:malloc(size);}
static uint64_t status_handle;
static float position[8200][4],colour[8200][4];
static float expected_tint;
static GLuint tint_location;
static bool check_textures;
static memory *get(uint64_t h){if(!h)return NULL;for(unsigned i=0;i<32;i++)if(pool[i].handle==h)return &pool[i];return NULL;}
rect_t rect_intersection(rect_t a,rect_t b){int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;return rect_make(x,y,r>x?r-x:0,d>y?d-y:0);}
rect_t rect_union(rect_t a,rect_t b){int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
    int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;return rect_make(x,y,r-x,d-y);}
bool rect_empty(rect_t r){return r.w<=0||r.h<=0;}
rect_t surface_clip(const surface_t *s){return rect_make(0,0,s->width,s->height);}
bool gui_gpu_flush(void){return true;}
bool gui_gpu_target(surface_t *s,bool depth,gui_gpu_target_t *out){
    if(s&&s->gpu&&s->gpu->invalid)return false;
    assert(s==g_gl.colour&&s->gpu&&!s->pixels&&!g_gl.depth&&!s->gpu->invalid);
    *out=(gui_gpu_target_t){900,depth?901:0,(unsigned)s->width*4,depth?(unsigned)s->width*4:0};return true;
}
void gui_gpu_invalidate(surface_t *s){assert(s==g_gl.colour);s->gpu->invalid=true;invalidations++;}
bool gui_gpu_cpu_access(surface_t *s){(void)s;assert(false);return false;}
bool gui_gpu_depth_readback(const surface_t *s,float *p,unsigned stride){(void)s;(void)p;(void)stride;assert(false);return false;}
bool gui_gpu_draw3d(surface_t *s,const surface_t *t,const kg3d_command_t *c,unsigned n,rect_t r){
    (void)s;(void)t;(void)c;(void)n;(void)r;assert(false);return false;
}
surface_t *gui_gpu_image(const colour_t *p,int w,int h){(void)p;(void)w;(void)h;assert(false);return NULL;}
void surface_destroy(surface_t *s){assert(!s);}
void log_write(int level,const char *tag,const char *note){(void)level;(void)tag;puts(note);}
void sh_run(sh_machine_t *m,const sh_shader_t *p){(void)m;(void)p;assert(false);}
intptr_t syscall6(intptr_t nr,intptr_t op,intptr_t ptr,intptr_t size,intptr_t a3,intptr_t a4,intptr_t a5){
    assert(nr==SYS_GPU&&op==GPUOP_SURFACE&&size==sizeof(kg2d_request_t)&&!a3&&!a4&&!a5);requests++;
    kg2d_request_t *r=(void*)ptr;
    if(r->operation==KG2D_CREATE){
        memory *m=NULL;for(unsigned i=0;i<32;i++)if(!pool[i].handle){m=&pool[i];break;}assert(m);
        m->bytes=(uint64_t)r->width*r->height*4;m->handle=++next_handle;m->data=calloc(1,(size_t)m->bytes);assert(m->data);
        r->handle=m->handle;r->pitch=r->width*4;r->bytes=m->bytes;creates++;return 0;
    }
    if(r->operation==KG2D_DESTROY){memory *m=get(r->handle);assert(m);free(m->data);*m=(memory){0};destroys++;return 0;}
    if(r->operation==KG2D_UPLOAD||r->operation==KG2D_DOWNLOAD){
        memory *m=get(r->handle);assert(m&&r->offset+r->bytes<=m->bytes);
        if(r->operation==KG2D_UPLOAD){
            if(transfer_texture){if(fail_texture)return -EBUSY;texture_uploads++;}
            memcpy(m->data+r->offset,(void*)(uintptr_t)r->data,(size_t)r->bytes);
        }else {if(r->handle!=status_handle)vertex_readbacks++;
            memcpy((void*)(uintptr_t)r->data,m->data+r->offset,(size_t)r->bytes);}
        return 0;
    }
    if(r->operation==KG2D_SHADER_VM){
        const ksh_dispatch_t *j=(const void*)(uintptr_t)r->data;assert(r->bytes==sizeof(*j)&&j->lanes&&j->lanes<=KSH_MAX_LANES);
        memory *regs=get(r->handle),*status=get(r->offset);assert(regs&&status);
        status_handle=r->offset;vertex_launches++;vertex_lanes+=j->lanes;if(fail_vertex)return -EIO;
        sh_shader_t shader={.compiled=true,.count=(int)j->code_count};memcpy(shader.code,j->code,sizeof shader.code);
        float *data=(void*)regs->data;in_model=true;
        for(unsigned lane=0;lane<j->lanes;lane++){
            sh_machine_t m={0};for(unsigned reg=0;reg<SR_REGISTERS;reg++)for(unsigned k=0;k<4;k++)m.reg[reg][k]=data[(reg*4+k)*j->lanes+lane];
            ref_sh_run(&m,&shader);assert(m.executed<=(int)j->budget);
            for(unsigned reg=0;reg<SR_REGISTERS;reg++)for(unsigned k=0;k<4;k++)data[(reg*4+k)*j->lanes+lane]=m.reg[reg][k];
            ((ksh_status_t*)status->data)[lane]=(ksh_status_t){m.discarded?KSH_DISCARDED:KSH_COMPLETE,(unsigned)m.executed};
        }in_model=false;return 0;
    }
    if(r->operation==KG2D_SHADER_GEOMETRY){
        assert(r->bytes==sizeof(kshs_submission_t)&&r->handle==900&&r->offset==901);
        const kshs_submission_t *s=(const void*)(uintptr_t)r->data;
        const kshr_job_t *j=&s->fragment;
        uint64_t handles[]={s->registers,s->vertex_status,s->workspace,s->raster_scratch,s->raster_status};
        for(unsigned i=0;i<5;i++){assert(get(handles[i]));for(unsigned k=0;k<i;k++)assert(handles[i]!=handles[k]);}
        assert(s->vertex_status==status_handle&&!j->command_count);
        assert(s->setup.lanes<=4095&&s->setup.triangle_count==s->setup.lanes/3);
        assert(s->setup.viewport_x==g_gl.vp_x&&s->setup.viewport_y==g_gl.vp_y);
        assert(s->setup.viewport_w==g_gl.vp_w&&s->setup.viewport_h==g_gl.vp_h);
        assert(s->setup.depth_near==g_gl.depth_near&&s->setup.depth_far==g_gl.depth_far);
        kshs_storage_layout_t layout;assert(kshs_storage_layout(s->setup.triangle_count,&layout));
        assert(get(s->workspace)->bytes>=layout.bytes);
        assert(get(s->raster_scratch)->bytes>=KSHR_JOB_ALLOCATION_BYTES(j));
        const sh_program_t *program=gl_current_program();
        assert(j->code_count==(unsigned)program->fragment->count);
        assert(!memcmp(j->code,program->fragment->code,j->code_count*16));
        assert(j->seed[program->fragment_reg[tint_location]][0]==expected_tint);
        if(check_textures){
            memory *tex=get(r->source);assert(j->texture_count==4);uint64_t offset=0;
            for(unsigned u=0;u<4;u++){
                const ksh_texture_t *d=&j->textures[u];
                const gl_texture_t *t=&g_gl.textures[g_gl.unit_texture[u]];
                if(!t->used||!t->texels){assert(!d->width&&!d->height);continue;}
                assert(tex&&d->offset==offset&&d->width==(unsigned)t->width&&d->height==(unsigned)t->height&&d->pitch==d->width);
                assert(d->filter==(t->mag_filter==GL_NEAREST?0u:1u));
                assert(d->wrap_s==(t->wrap_s==GL_REPEAT?0u:1u)&&d->wrap_t==(t->wrap_t==GL_REPEAT?0u:1u));
                uint64_t bytes=(uint64_t)t->width*t->height*4;
                assert(offset+bytes<=tex->bytes&&!memcmp(tex->data+offset,t->texels,(size_t)bytes));offset+=bytes;
            }
            assert((offset!=0)==(tex!=NULL));
        }else assert(!r->source);
        geometry_launches++;if(fail_geometry)return -EIO;
        unsigned emitted=0;
        assert(test_model_setup(s,(const float*)get(s->registers)->data,
            (const ksh_status_t*)get(s->vertex_status)->data,get(s->workspace)->data,
            observed+records,20000-records,&emitted));
        records+=emitted;r->count=emitted;
        r->bytes=(uint64_t)emitted*j->code_count; // modeled count, not GPU timing/pixels
        return 0;
    }
    assert(r->operation==KG2D_SHADER_RASTER&&r->bytes==sizeof(kshr_submission_t));
    const kshr_submission_t *s=(const void*)(uintptr_t)r->data;const kshr_job_t *j=&s->job;
    assert(r->handle==900&&r->offset==901&&j->pitch==j->width&&j->depth_pitch==j->width);
    assert(j->clip_w&&j->clip_h&&j->clip_x+j->clip_w<=j->width&&j->clip_y+j->clip_h<=j->height);
    unsigned n=kshr_job_lanes(j);assert(n<=KSHR_FAST_MAX_LANES);
    memory *scratch=get(s->scratch),*status=get(s->status);assert(scratch&&status&&scratch->bytes>=KSHR_JOB_ALLOCATION_BYTES(j));
    assert(kshr_region_cost(j->commands,j->command_count,j->budget,j->clip_x,j->clip_y,j->clip_w,j->clip_h)<=KSH_MAX_WORK);
    const sh_program_t *program=gl_current_program();assert(j->code_count==(unsigned)program->fragment->count);
    assert(!memcmp(j->code,program->fragment->code,j->code_count*16));
    assert(j->seed[program->fragment_reg[tint_location]][0]==expected_tint);
    if(check_textures){
        memory *tex=get(r->source);assert(j->texture_count==4);uint64_t offset=0;
        for(unsigned u=0;u<4;u++){const ksh_texture_t *d=&j->textures[u];
            const gl_texture_t *t=&g_gl.textures[g_gl.unit_texture[u]];
            if(!t->used||!t->texels){assert(!d->width&&!d->height);continue;}
            assert(tex&&d->offset==offset&&d->width==(unsigned)t->width&&d->height==(unsigned)t->height&&d->pitch==d->width);
            assert(d->filter==(t->mag_filter==GL_NEAREST?0u:1u));
            assert(d->wrap_s==(t->wrap_s==GL_REPEAT?0u:1u)&&d->wrap_t==(t->wrap_t==GL_REPEAT?0u:1u));
            uint64_t bytes=(uint64_t)t->width*t->height*4;
            assert(offset+bytes<=tex->bytes&&!memcmp(tex->data+offset,t->texels,(size_t)bytes));offset+=bytes;
        }
        assert((offset!=0)==(tex!=NULL));
    }else assert(!r->source);
    rect_t bounds=rect_make(j->commands[0].raster.x,j->commands[0].raster.y,j->commands[0].raster.width,j->commands[0].raster.height);
    for(unsigned i=1;i<j->command_count;i++)bounds=rect_union(bounds,rect_make(j->commands[i].raster.x,j->commands[i].raster.y,j->commands[i].raster.width,j->commands[i].raster.height));
    if(j->clip_x==(unsigned)bounds.x&&j->clip_y==(unsigned)bounds.y){
        for(unsigned i=0;i<j->command_count;i++){assert(records<20000);observed[records++]=j->commands[i];}
    }
    raster_launches++;if(fail_raster)return -EIO;
    for(unsigned i=0;i<n;i++)((ksh_status_t*)status->data)[i]=(ksh_status_t){KSH_COMPLETE,j->code_count};
    if(bad_status)((ksh_status_t*)status->data)[n-1].result=KSH_PENDING;
    status_handle=s->status;return 0;
}
static GLuint shader(GLenum stage,const char *source){
    GLuint id=glCreateShader(stage);glShaderSource(id,1,&source,NULL);glCompileShader(id);
    GLint ok;glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[512];glGetShaderInfoLog(id,512,NULL,log);puts(log);}assert(ok);return id;
}
static unsigned index_of(const kshr_command_t *cmd,unsigned v){
    return (unsigned)lroundf(cmd->varying[v][0][0]*1024.f/cmd->raster.v[v].inv_w);
}
static void check_tri(unsigned at,unsigned a,unsigned b,unsigned c){
    assert(index_of(&observed[at],0)==a&&index_of(&observed[at],1)==b&&index_of(&observed[at],2)==c);
}
static void program_lifetime(void){
    GLuint v=shader(GL_VERTEX_SHADER,"attribute vec4 position; void main(){gl_Position=position;}");
    GLuint f=shader(GL_FRAGMENT_SHADER,"uniform vec4 tint; void main(){gl_FragColor=tint;}");
    GLuint a=glCreateProgram(),b=glCreateProgram();
    glAttachShader(a,v);glAttachShader(a,f);glAttachShader(b,v);glAttachShader(b,f);
    glLinkProgram(a);glLinkProgram(b);assert(glGetError()==GL_NO_ERROR);
    gl_program_object_t *pa=&g_gl.program_object[a-1],*pb=&g_gl.program_object[b-1];
    assert(pa->linked&&pb->linked&&pa->executable!=pb->executable);
    assert(pa->program.fragment!=&g_gl.shader_object[f-1].shader);
    glUseProgram(a);GLint loc=glGetUniformLocation(a,"tint");glUniform4f(loc,.125f,.25f,.5f,1);
    glUseProgram(b);glUniform4f(glGetUniformLocation(b,"tint"),1,.75f,.5f,.25f);
    assert(pa->uniform_value[loc][0]==.125f&&pb->uniform_value[loc][0]==1);
    // Compile/link must never patch shared source bytecode in another program.
    sh_shader_t saved=*pa->program.fragment;
    const char *different="void main(){gl_FragColor=vec4(0.3,0.2,0.1,1.0);}";
    glShaderSource(f,1,&different,NULL);glCompileShader(f);
    assert(!memcmp(&saved,pa->program.fragment,sizeof saved));
    glLinkProgram(b);assert(pb->linked&&!memcmp(&saved,pa->program.fragment,sizeof saved));
    for(unsigned i=0;i<SH_MAX_SYMBOLS*2;i++)for(unsigned k=0;k<16;k++)assert(!pb->uniform_value[i][k]);
    glDeleteShader(v);glDeleteShader(f);
    assert(g_gl.shader_object[v-1].used&&g_gl.shader_object[v-1].attachments==2);
    glDetachShader(a,v);glDetachShader(a,f);assert(pa->linked&&g_gl.shader_object[v-1].used);
    glUseProgram(a);assert(gl_current_program()==&pa->program);
    // A failed relink preserves the installed executable, not its link status.
    glLinkProgram(a);assert(!pa->linked&&gl_current_program()==&pa->program);
    assert(!memcmp(&saved,pa->program.fragment,sizeof saved)&&pa->uniform_value[loc][0]==.125f);
    glUseProgram(0);glUseProgram(a);assert(glGetError()==GL_INVALID_OPERATION&&!gl_current_program());
    glDeleteProgram(a);assert(!pa->used&&!pa->executable);
    glUseProgram(b);glDeleteProgram(b);assert(pb->used&&pb->delete_pending&&gl_current_program());
    glUseProgram(0);assert(!pb->used&&!pb->executable);
    assert(!g_gl.shader_object[v-1].used&&!g_gl.shader_object[f-1].used);
    glUniform1f(0,1);assert(glGetError()==GL_INVALID_OPERATION);
    glUniform1f(-1,1);assert(glGetError()==GL_NO_ERROR);
    puts("PASS linked executable isolation, per-program/reset uniforms, attachment references, deferred deletion, failed installed relink, complete source/executable cleanup");
}
static void cpu_span_boundary(void){
    colour_t pixels[64]={0};surface_t canvas={.pixels=pixels,.width=8,.height=8,.stride=8};
    unsigned before=requests;
    glSetTarget(&canvas);glViewport(0,0,8,8);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,8,8,0,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);
    glClearColor(0,0,0,1);glColor3f(1,0,0);
    for(unsigned half=0;half<2;half++){
        glClear(GL_COLOR_BUFFER_BIT);glBegin(GL_TRIANGLES);
        glVertex3f(1,1,0);
        if(!half){glVertex3f(7,1,0);glVertex3f(7,7,0);}
        else {glVertex3f(7,7,0);glVertex3f(1,7,0);}
        glEnd();glFinish();
        for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){
            bool inside=x>=1&&x<7&&y>=1&&y<7&&(half?x<y:x>=y);
            assert((pixels[y*8+x]&0xffffffu)==(inside?0xff0000u:0));
        }
    }
    assert(glGetError()==GL_NO_ERROR&&requests==before);glSetTarget(NULL);glGetStats(NULL,NULL);
    puts("PASS CPU reference span: exact owned integral crossings, complementary top-left edges, no shared-edge holes or overlap");
}
int main(int argc,char **argv){
    program_lifetime();cpu_span_boundary();
    struct gui_gpu_surface native={1,false};surface_t canvas={.width=96,.height=64,.stride=96,.owns_pixels=true,.gpu=&native};
    glSetTarget(&canvas);glViewport(0,0,96,64);glEnable(GL_DEPTH_TEST);
    GLuint v=shader(GL_VERTEX_SHADER,"attribute vec4 position; attribute vec4 colour; uniform mat4 transform; varying vec4 value; void main(){gl_Position=transform*position; value=colour;}");
    GLuint f=shader(GL_FRAGMENT_SHADER,"varying vec4 value; uniform vec4 tint; void main(){gl_FragColor=value*tint;}");
    GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(p);
    GLint pa=glGetAttribLocation(p,"position"),ca=glGetAttribLocation(p,"colour");assert(pa>=0&&ca>=0);
    float matrix[16]={.5f,0,0,0,0,.5f,0,0,0,0,1,0,.125f,-.125f,0,1};
    glUniformMatrix4fv(glGetUniformLocation(p,"transform"),1,GL_FALSE,matrix);
    tint_location=glGetUniformLocation(p,"tint");expected_tint=.75f;glUniform4f(tint_location,.75f,.5f,.25f,1);
    for(unsigned i=0;i<8200;i++){
        position[i][0]=(i%3==0?-.75f:i%3==1?.75f:0);position[i][1]=(i%3==2?.75f:-.5f);position[i][3]=1;
        colour[i][0]=i/1024.f;colour[i][1]=.25f;colour[i][2]=.5f;colour[i][3]=1;
    }
    glVertexAttribPointer(pa,4,GL_FLOAT,GL_FALSE,0,position);glEnableVertexAttribArray(pa);
    glVertexAttribPointer(ca,4,GL_FLOAT,GL_FALSE,0,colour);glEnableVertexAttribArray(ca);
    glDrawArrays(GL_TRIANGLES,0,771);
    assert(glGetError()==GL_NO_ERROR&&!invalidations&&vertex_launches==1&&vertex_lanes==771&&records==257);
    assert(geometry_launches==1&&!vertex_readbacks&&!raster_launches);
    assert(!canvas.pixels&&!g_gl.depth&&canvas.gpu&&g_gl.gpu_target);
    for(unsigned i=0;i<257;i++)check_tri(i,i*3,i*3+1,i*3+2);
    assert(observed[0].raster.v[0].x==36 && observed[0].raster.v[0].y==44);
    unsigned base=records;glDrawArrays(GL_TRIANGLE_STRIP,2,515);
    assert(records-base==513);for(unsigned i=0;i<513;i++)check_tri(base+i,2+i+(i&1),3+i-(i&1),4+i);
    base=records;glDrawArrays(GL_TRIANGLE_FAN,4,513);assert(records-base==511);
    for(unsigned i=0;i<511;i++)check_tri(base+i,4,5+i,6+i);
    base=records;glDrawArrays(GL_QUADS,0,516);assert(records-base==258);
    for(unsigned i=0;i<129;i++){check_tri(base+i*2,i*4,i*4+1,i*4+2);check_tri(base+i*2+1,i*4,i*4+2,i*4+3);}
    base=records;glDrawArrays(GL_QUAD_STRIP,0,518);assert(records-base==516);
    for(unsigned i=0;i<258;i++){check_tri(base+i*2,i*2,i*2+1,i*2+3);check_tri(base+i*2+1,i*2,i*2+3,i*2+2);}
    base=records;glDrawArrays(GL_LINE_LOOP,3,514);assert(records-base==514);
    for(unsigned i=0;i<514;i++){assert(index_of(&observed[base+i],0)==3+i);assert(index_of(&observed[base+i],1)==3+(i+1)%514);}
    base=records;glDrawArrays(GL_POINTS,0,513);assert(records-base==513);
    for(unsigned i=0;i<513;i++)assert(index_of(&observed[base+i],0)==i);
    unsigned short indices[519];for(unsigned i=0;i<519;i++)indices[i]=518-i;
    base=records;glDrawElements(GL_TRIANGLES,519,GL_UNSIGNED_SHORT,indices);assert(records-base==173);
    for(unsigned i=0;i<173;i++)check_tri(base+i,518-i*3,517-i*3,516-i*3);
    // Immediate generic attributes must produce the identical ordered commands
    // as array draws, with no CPU shaders, client-array fetches or CPU pixels.
    // 259 vertices crosses a full batch and leaves a non-power-of-two tail.
    assert(pa==0);
    for(GLenum mode=GL_POINTS;mode<=GL_POLYGON;mode++){
        unsigned reference=records;glDrawArrays(mode,0,259);unsigned n=records-reference;
        unsigned immediate=records,launches=vertex_launches,lanes=vertex_lanes;
        unsigned readbacks=vertex_readbacks,geometries=geometry_launches;
        gl_array_t saved_pa=g_gl.attrib_array[pa],saved_ca=g_gl.attrib_array[ca];
        g_gl.attrib_array[pa].pointer=(void *)(uintptr_t)1;
        g_gl.attrib_array[ca].pointer=(void *)(uintptr_t)1; // must never be dereferenced
        glBegin(mode);
        for(unsigned i=0;i<259;i++){
            glVertexAttrib4f(ca,colour[i][0],colour[i][1],colour[i][2],colour[i][3]);
            if(i&1)glVertexAttrib4f(0,position[i][0],position[i][1],position[i][2],position[i][3]);
            else glVertex4f(position[i][0],position[i][1],position[i][2],position[i][3]);
        }
        // A later current-attribute change cannot recolour earlier vertices.
        glVertexAttrib4f(ca,999,999,999,999);glEnd();
        g_gl.attrib_array[pa]=saved_pa;g_gl.attrib_array[ca]=saved_ca;
        assert(glGetError()==GL_NO_ERROR && records-immediate==n);
        assert(vertex_launches-launches==2 && vertex_lanes-lanes==(mode==GL_TRIANGLES?258:259));
        if(mode==GL_TRIANGLES)assert(vertex_readbacks==readbacks&&geometry_launches-geometries==2);
        assert(!memcmp(observed+reference,observed+immediate,n*sizeof(*observed)));
        assert(!canvas.pixels && !g_gl.depth && !native.invalid && !gl_shader_gpu_active() && !g_gl.in_begin);
    }
    // Beyond the former 4096-vertex staging limit: no dropped incomplete
    // triangle at the limit, no repeated vertices, bounded 255-lane storage.
    base=records;unsigned immediate_launches=vertex_launches;
    glBegin(GL_TRIANGLES);
    for(unsigned i=0;i<4101;i++){
        unsigned at=i%1030;
        glVertexAttrib4f(ca,colour[at][0],colour[at][1],colour[at][2],colour[at][3]);
        glVertex4f(position[at][0],position[at][1],position[at][2],position[at][3]);
    }
    glEnd();assert(records-base==1367 && vertex_launches-immediate_launches==17 && glGetError()==GL_NO_ERROR);
    for(unsigned i=0;i<1367;i++)check_tri(base+i,(i*3)%1030,(i*3+1)%1030,(i*3+2)%1030);
    // Resident immediate tails compact from the CAPTURED 255-lane stride,
    // never 256 or the current storage capacity left by an earlier array draw.
    const unsigned immediate_counts[]={1,2,3,254,255,256,257,258,509,510,511};
    for(unsigned c=0;c<sizeof immediate_counts/sizeof(*immediate_counts);c++){
        unsigned count=immediate_counts[c],consumed=count-count%3;
        unsigned before=records,lanes=vertex_lanes,launches=vertex_launches;
        unsigned readbacks=vertex_readbacks,geometries=geometry_launches,rasters=raster_launches;
        glBegin(GL_TRIANGLES);
        for(unsigned i=0;i<count;i++){
            glVertexAttrib4f(ca,colour[i][0],colour[i][1],colour[i][2],colour[i][3]);
            glVertex4f(position[i][0],position[i][1],position[i][2],position[i][3]);
        }
        glVertexAttrib4f(ca,999,999,999,999);glEnd();
        assert(glGetError()==GL_NO_ERROR&&!native.invalid&&records-before==consumed/3);
        assert(vertex_lanes-lanes==consumed&&vertex_launches-launches==(consumed+254)/255);
        assert(geometry_launches-geometries==(consumed+254)/255&&raster_launches==rasters&&vertex_readbacks==readbacks);
        for(unsigned i=0;i<consumed/3;i++)check_tri(before+i,i*3,i*3+1,i*3+2);
    }
    // Empty and incomplete groups never dispatch fragments. Reject re-entrant
    // draws and target/program/uniform mutation without damaging the open draw.
    base=records;glBegin(GL_TRIANGLES);glEnd();assert(records==base && glGetError()==GL_NO_ERROR);
    glBegin(GL_POINTS);
    glBegin(GL_LINES);assert(glGetError()==GL_INVALID_OPERATION && g_gl.primitive==GL_POINTS);
    glUseProgram(0);assert(glGetError()==GL_INVALID_OPERATION && g_gl.bound_program==p);
    glLinkProgram(p);assert(glGetError()==GL_INVALID_OPERATION);
    glUniform4f(tint_location,0,0,0,0);assert(glGetError()==GL_INVALID_OPERATION);
    glSetTarget(NULL);assert(glGetError()==GL_INVALID_OPERATION && g_gl.colour==&canvas);
    glDrawArrays(GL_TRIANGLES,0,3);assert(glGetError()==GL_INVALID_OPERATION);
    glFinish();assert(glGetError()==GL_INVALID_OPERATION);
    glClear(GL_COLOR_BUFFER_BIT);assert(glGetError()==GL_INVALID_OPERATION);
    glViewport(0,0,1,1);assert(glGetError()==GL_INVALID_OPERATION && g_gl.vp_w==96);
    glVertexAttrib4f(ca,colour[0][0],colour[0][1],colour[0][2],colour[0][3]);
    glVertex4f(position[0][0],position[0][1],position[0][2],position[0][3]);glEnd();
    assert(records==base+1 && glGetError()==GL_NO_ERROR);
    glBegin(0xffff);assert(glGetError()==GL_INVALID_ENUM && !g_gl.in_begin);
    glEnd();assert(glGetError()==GL_INVALID_OPERATION);
    puts("PASS GPU immediate shaders: all ten topologies match array commands, per-vertex generic attributes/attribute-zero emission, ignored client arrays, 4101 vertices, partial batches and state guards; no CPU shaders or pixels");
    // Growing either CPU input/output staging allocation must fail before GPU
    // work, preserve the existing staging, and allow a clean retry afterward.
    for(unsigned failure=1;failure<=2;failure++){
        unsigned before=vertex_launches,draws=raster_launches,allocs=creates;
        staging_allocations=0;staging_fail_at=failure;
        glDrawArrays(GL_POINTS,0,4096); // legacy route still needs both input and output staging
        assert(glGetError()==GL_OUT_OF_MEMORY && native.invalid && !gl_shader_gpu_active());
        assert(vertex_launches==before && raster_launches==draws && creates==allocs);
        staging_fail_at=0;native.invalid=false;
        glDrawArrays(GL_TRIANGLES,0,3);assert(glGetError()==GL_NO_ERROR && !native.invalid);
    }
    invalidations=0;
    // Compare large array batches with the bounded 255/256-lane immediate route.
    // Exercise both sides of the new ceiling, a second full batch and odd tail,
    // all topology parity/closure rules, nonzero first and reversed indices.
    const unsigned batch_counts[]={4095,4096,4097,8195};
    unsigned large_indices[8195];
    for(unsigned c=0;c<4;c++)for(GLenum mode=GL_POINTS;mode<=GL_POLYGON;mode++){
        unsigned count=batch_counts[c];records=0;
        unsigned launches=vertex_launches,lanes=vertex_lanes;
        glDrawArrays(mode,3,count);
        unsigned consumed=mode==GL_TRIANGLES?count-count%3:count;
        unsigned batch=mode==GL_TRIANGLES?4095:4096;
        assert(glGetError()==GL_NO_ERROR && vertex_launches-launches==(consumed+batch-1)/batch);
        assert(vertex_lanes-lanes==consumed && !native.invalid);
        unsigned n=records;
        glBegin(mode);
        for(unsigned i=0;i<count;i++){
            unsigned at=i+3;
            glVertexAttrib4f(ca,colour[at][0],colour[at][1],colour[at][2],colour[at][3]);
            glVertex4f(position[at][0],position[at][1],position[at][2],position[at][3]);
        }
        glEnd();assert(glGetError()==GL_NO_ERROR && records==2*n);
        assert(!memcmp(observed,observed+n,n*sizeof(*observed)));
        records=0;launches=vertex_launches;
        for(unsigned i=0;i<count;i++)large_indices[i]=count+2-i;
        glDrawElements(mode,count,GL_UNSIGNED_INT,large_indices);n=records;
        assert(glGetError()==GL_NO_ERROR && vertex_launches-launches==(consumed+batch-1)/batch);
        glBegin(mode);
        for(unsigned i=0;i<count;i++){
            unsigned at=large_indices[i];
            glVertexAttrib4f(ca,colour[at][0],colour[at][1],colour[at][2],colour[at][3]);
            glVertex4f(position[at][0],position[at][1],position[at][2],position[at][3]);
        }
        glEnd();assert(glGetError()==GL_NO_ERROR && records==2*n);
        assert(!memcmp(observed,observed+n,n*sizeof(*observed)));
        assert(!canvas.pixels && !g_gl.depth && !native.invalid && !gl_shader_gpu_active());
    }
    records=0;
    puts("PASS bounded array batching: 80 array/indexed boundary draws, all topologies identical to 255/256-lane immediate reference, exact dispatch counts, nonzero first/reversed indices, 8195-vertex tails");
    // Uniforms/descriptors are fresh; immutable image bytes remain resident.
    GLuint tex[4];glGenTextures(4,tex);
    for(unsigned u=0;u<4;u++){
        unsigned char pixel[4]={0x10+u,0x20+u,0x30+u,0xff};
        glActiveTexture(GL_TEXTURE0+u);glBindTexture(GL_TEXTURE_2D,tex[u]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    }
    check_textures=true;expected_tint=.125f;glUniform4f(tint_location,.125f,.5f,.25f,1);
    base=texture_uploads;unsigned old_raster=raster_launches,old_geometry=geometry_launches;
    // Cross the current 4095-vertex triangle batch, retaining the check that
    // immutable textures upload only once across multiple raster dispatches.
    glDrawArrays(GL_TRIANGLES,0,4098);assert(texture_uploads==base+1&&geometry_launches==old_geometry+2&&raster_launches==old_raster&&glGetError()==GL_NO_ERROR);
    base=texture_uploads;
    for(unsigned repeat=0;repeat<3;repeat++)glDrawArrays(GL_TRIANGLES,0,3);
    assert(texture_uploads==base&&glGetError()==GL_NO_ERROR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==base&&glGetError()==GL_NO_ERROR);
    uint64_t version=g_gl.textures[tex[3]].content_version;
    unsigned char replacement[8]={200,100,50,255,30,60,90,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,replacement);
    assert(g_gl.textures[tex[3]].content_version!=version);
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==++base&&glGetError()==GL_NO_ERROR);
    colour_t *unchanged=g_gl.textures[tex[3]].texels;version=g_gl.textures[tex[3]].content_version;
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,1,0,GL_RGBA,GL_FLOAT,replacement);
    assert(glGetError()==GL_INVALID_ENUM);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,1,0,0xffff,GL_UNSIGNED_BYTE,NULL);
    assert(glGetError()==GL_INVALID_ENUM&&g_gl.textures[tex[3]].texels==unchanged&&g_gl.textures[tex[3]].content_version==version);
    fail_texture_alloc=true;
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,1,0,GL_RGBA,GL_UNSIGNED_BYTE,replacement);
    assert(glGetError()==GL_OUT_OF_MEMORY&&g_gl.textures[tex[3]].texels==unchanged&&g_gl.textures[tex[3]].content_version==version);
    fail_texture_alloc=false;
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==base&&glGetError()==GL_NO_ERROR);
    glActiveTexture(GL_TEXTURE0+2);
    assert(g_gl.bound_texture==tex[2]);
    uint64_t other_version=g_gl.textures[tex[3]].content_version;
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,1,0,GL_RGBA,GL_UNSIGNED_BYTE,replacement);
    assert(g_gl.textures[tex[2]].width==2&&g_gl.textures[tex[3]].width==1&&g_gl.textures[tex[3]].content_version==other_version);
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==++base&&glGetError()==GL_NO_ERROR);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,tex[2]);
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==++base&&glGetError()==GL_NO_ERROR);
    version=g_gl.textures[tex[2]].content_version;GLuint old_name=tex[2];
    glDeleteTextures(1,&old_name);assert(!g_gl.unit_texture[0]&&!g_gl.unit_texture[2]&&!g_gl.bound_texture);
    glGenTextures(1,&tex[2]);assert(tex[2]==old_name); // name/address ABA must not reuse old image bytes
    glActiveTexture(GL_TEXTURE0+2);glBindTexture(GL_TEXTURE_2D,tex[2]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    assert(g_gl.textures[tex[2]].content_version!=version&&!g_gl.unit_texture[0]);
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==++base&&glGetError()==GL_NO_ERROR);
    for(unsigned u=0;u<4;u++){glActiveTexture(GL_TEXTURE0+u);glBindTexture(GL_TEXTURE_2D,0);}
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==base&&creates-destroys==5&&glGetError()==GL_NO_ERROR);
    for(unsigned u=0;u<4;u++){glActiveTexture(GL_TEXTURE0+u);glBindTexture(GL_TEXTURE_2D,tex[u]);}
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==++base&&glGetError()==GL_NO_ERROR);
    puts("PASS texture residency: repeated draws, fresh sampler state, changed image/dimensions, rejected uploads, deleted multi-unit bindings/name reuse, zero image and unused atlas release");
    version=g_gl.textures[tex[3]].content_version;g_gl.textures[tex[3]].content_version=0;
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==++base&&glGetError()==GL_NO_ERROR);
    glDrawArrays(GL_TRIANGLES,0,3);assert(texture_uploads==++base&&glGetError()==GL_NO_ERROR);
    g_gl.textures[tex[3]].content_version=version;
    // Generic attributes honor normalized=false and every integer signedness.
    unsigned char ub[4]={128,64,32,255};signed char sb[4]={-128,127,0,64};
    unsigned short us[4]={65535,32768,0,16384};short ss[4]={-32768,32767,0,16384};
    unsigned ui[4]={0xffffffffu,0x80000000u,0,1};int si[4]={INT32_MIN,INT32_MAX,0,1};
    const void *arrays[6]={ub,sb,us,ss,ui,si};GLenum types[6]={GL_UNSIGNED_BYTE,GL_BYTE,GL_UNSIGNED_SHORT,GL_SHORT,GL_UNSIGNED_INT,GL_INT};
    const float raw[6]={128,-128,65535,-32768,4294967295.0f,-2147483648.0f};
    const float normalized[6]={128.f/255,-1,1,-1,1,-1};
    int varying_reg=gl_current_program()->attribute[ca].reg;
    for(unsigned t=0;t<6;t++)for(unsigned norm=0;norm<2;norm++){
        float input[SR_REGISTERS*4]={0};
        glVertexAttribPointer(ca,4,types[t],norm,0,arrays[t]);
        assert(gl_shader_vertex_input(0,input,1,0));
        assert(input[varying_reg*4]==(norm?normalized[t]:raw[t]));
    }
    glDisableVertexAttribArray(ca);glVertexAttrib3f(ca,.125f,.25f,.5f);
    float constant_input[SR_REGISTERS*4]={0};assert(gl_shader_vertex_input(0,constant_input,1,0));
    assert(constant_input[varying_reg*4]==.125f&&constant_input[varying_reg*4+3]==1);
    glVertexAttribPointer(ca,4,GL_FLOAT,GL_FALSE,-1,colour);assert(glGetError()==GL_INVALID_VALUE);
    glVertexAttribPointer(ca,4,0xffff,GL_FALSE,0,colour);assert(glGetError()==GL_INVALID_ENUM);
    glVertexAttribPointer(ca,4,GL_FLOAT,GL_FALSE,0,colour);glEnableVertexAttribArray(ca);
    unsigned old_requests=requests;glDrawArrays(GL_TRIANGLES,-1,3);assert(glGetError()==GL_INVALID_VALUE&&requests==old_requests);
    unsigned bad_index=0xffffffff;glDrawElements(GL_POINTS,1,GL_UNSIGNED_INT,&bad_index);
    assert(glGetError()==GL_INVALID_OPERATION&&invalidations==1&&!canvas.pixels);native.invalid=false;
    // Invalid lane output must suppress the canvas, not demote it to CPU.
    bad_status=true;glDrawArrays(GL_POINTS,0,3);assert(glGetError()==GL_INVALID_OPERATION&&invalidations==2&&native.invalid);
    bad_status=false;native.invalid=false;
    glDrawArrays(GL_TRIANGLES,0,3);assert(glGetError()==GL_NO_ERROR&&!native.invalid);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,replacement);
    base=texture_uploads;unsigned before_failure=vertex_launches;
    fail_texture=true;glDrawArrays(GL_TRIANGLES,0,3);
    assert(glGetError()==GL_INVALID_OPERATION&&native.invalid&&vertex_launches==before_failure&&texture_uploads==base);
    fail_texture=false;native.invalid=false;glDrawArrays(GL_TRIANGLES,0,3);
    assert(glGetError()==GL_NO_ERROR&&texture_uploads==base+1);
    glDrawArrays(GL_TRIANGLES,0,3);assert(glGetError()==GL_NO_ERROR&&texture_uploads==base+1);
    puts("PASS failed atlas upload: no vertex dispatch or valid canvas, retry uploads afresh, subsequent draw reuses only successful upload");
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,replacement);
    before_failure=vertex_launches;unsigned failed_before=invalidations;
    fail_texture=true;glBegin(GL_TRIANGLES);
    for(unsigned i=0;i<300;i++)glVertex4f(0,0,0,1);
    glEnd();assert(glGetError()==GL_INVALID_OPERATION && native.invalid && invalidations==failed_before+1);
    assert(vertex_launches==before_failure && !gl_shader_gpu_active() && !g_gl.in_begin && !canvas.pixels);
    fail_texture=false;native.invalid=false;glDrawArrays(GL_TRIANGLES,0,3);
    assert(glGetError()==GL_NO_ERROR && !native.invalid);
    puts("PASS immediate startup failure: no vertex launch/CPU fallback, glEnd releases draw state, retry uses fresh texture data");
    glSetTarget(NULL);assert(creates==destroys); // idle scratch/atlas/status released
    unsigned before=invalidations;
    if(argc>1&&(!strcmp(argv[1],"geometry-failure")||!strcmp(argv[1],"immediate-geometry-failure"))){
    glSetTarget(&canvas);fail_geometry=true;
    unsigned before_geometry=geometry_launches,before_raster=raster_launches,before_reads=vertex_readbacks;
    if(!strcmp(argv[1],"immediate-geometry-failure")){
        unsigned launches=vertex_launches;
        glBegin(GL_TRIANGLES);
        for(unsigned i=0;i<600;i++){
            glVertexAttrib4f(ca,colour[i][0],colour[i][1],colour[i][2],colour[i][3]);
            glVertex4f(position[i][0],position[i][1],position[i][2],position[i][3]);
        }
        glEnd();assert(vertex_launches==launches+1&&!g_gl.in_begin&&!gl_shader_gpu_active());
    }else glDrawArrays(GL_TRIANGLES,0,3);
    assert(glGetError()==GL_INVALID_OPERATION&&native.invalid&&invalidations==before+1);
    assert(geometry_launches==before_geometry+1&&raster_launches==before_raster&&vertex_readbacks==before_reads);
    old_requests=requests;glDrawArrays(GL_TRIANGLES,0,3);
    assert(glGetError()==GL_INVALID_OPERATION&&requests==old_requests); // never replay failed op9
    old_requests=requests;glSetTarget(NULL);assert(requests==old_requests);
    /* The normal invocation covers legacy immediate raster failure below;
     * never clear the live owner's quarantine to continue another GPU test. */
    printf("PASS modeled resident route: %u geometry submissions; op9 failure invalidates target, no CPU readback/raster retry, quarantined resources retained\n",geometry_launches);
    return 0;
    }
    glSetTarget(&canvas);fail_raster=true;before=invalidations;
    glBegin(GL_POINTS);
    for(unsigned i=0;i<600;i++)glVertex4f(position[i%1030][0],position[i%1030][1],0,1);
    glEnd();assert(!g_gl.in_begin && !gl_shader_gpu_active());
    assert(glGetError()==GL_INVALID_OPERATION&&invalidations==before+1&&native.invalid);
    old_requests=requests;glSetTarget(NULL);assert(requests==old_requests); // unfenced resources retained
    printf("PASS actual programmable GL route: %u native vertex batches/%u vertices, %u primitive records/%u tiled raster calls; compiler/uniforms/attributes, topology continuity, texture reuse, no CPU shaders/pixels, completion/failure/lifetime gates (modeled GPU)\n",vertex_launches,vertex_lanes,records,raster_launches);
}
'''

if __name__=='__main__':
    header=HEADER
    for p in ['include/kestrel/gpu2d.h','include/kestrel/gpu3d.h','include/kestrel/shader_vm.h',
              'include/kestrel/shader_raster.h','include/kestrel/shader_setup.h','user/libgl/GL.h','user/libgl/shader.h',
              'user/libgl/glstate.h','user/libgl/gpuvm.h']:
        header+='\n'+strip_includes((ROOT/p).read_text())
    header+='\nbool gui_gpu_draw3d(surface_t*,const surface_t*,const kg3d_command_t*,unsigned,rect_t);\nvoid ref_sh_run(sh_machine_t*,const sh_shader_t*);\n#endif\n'
    clang=shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    with tempfile.TemporaryDirectory(prefix='kestrel-native-program-') as tmp:
        tmp=Path(tmp);(tmp/'test.h').write_text(header);files=[]
        for name in ['gl.c','glmath.c','gltex.c','raster.c','gpu.c','shaderapi.c','shadergpu.c','fixedgpu.c','gpuvm.c','glsl.c','shadervm.c']:
            src=strip_includes((ROOT/'user/libgl'/name).read_text())
            if name=='gltex.c':
                assert src.count('malloc(')==1
                src=src.replace('malloc(', 'test_texture_alloc(')
            if name=='shadergpu.c':
                for label in ('in','out'):
                    anchor=f'float *{label}=malloc('
                    assert src.count(anchor)==1
                    src=src.replace(anchor,f'float *{label}=test_staging_alloc(')
            if name=='shadervm.c':src=src.replace('void sh_run(', 'void ref_sh_run(')
            if name=='gpuvm.c':
                assert src.count('    unsigned char *p=data;')==1
                src=src.replace('    unsigned char *p=data;', '    test_vm_transfer_role(buffer==&vm->textures);\n    unsigned char *p=data;')
            path=tmp/name;path.write_text('#include "test.h"\n'+src);files.append(str(path))
        (tmp/'test.c').write_text(HARNESS);exe=tmp/'test.exe'
        model=tmp/'setup_model.obj'
        subprocess.run([clang,'-std=c++17','-O1','-ffp-contract=off','-c',str(ROOT/'tools/shader_setup_host_model.cpp'),'-o',str(model)],check=True)
        subprocess.run([clang,'-std=c11','-O1','-Wall','-Wextra',*files,str(tmp/'test.c'),str(model),'-o',str(exe)],check=True)
        if '--compile-only' in sys.argv[1:]:print('COMPILED ONLY GL shader model: executable was not run')
        else:
            subprocess.run([str(exe)],check=True)
            subprocess.run([str(exe),'geometry-failure'],check=True)
            subprocess.run([str(exe),'immediate-geometry-failure'],check=True)
