#!/usr/bin/env python3
"""Fixed-function vertex and resident geometry routing with modeled GPU boundaries.

CPU transforms are allowed only for the independent reference comparisons.
Application draws must dispatch the actual generated shader, not that reference.
The syscall model uses host setup and bytecode oracles; it is not native GPU
execution, pixel rendering, or a performance measurement. --compile-only builds
the harness and setup model without running either.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_gl_shader_gpu import HEADER, HARNESS, ROOT
from test_gl_gpu import strip_includes

# Model only the syscall boundary; production has no CPU setup/raster fallback.
FIXED_GEOMETRY = r'''
    if(r->operation==KG2D_SHADER_GEOMETRY){
        assert(!gl_current_program() && r->bytes==sizeof(kshs_submission_t));
        assert(r->handle==900 && r->offset==(g_gl.depth_test?901u:0u));
        const kshs_submission_t *s=(const void*)(uintptr_t)r->data;
        const kshr_job_t *j=&s->fragment;
        uint64_t handles[]={s->registers,s->vertex_status,s->workspace,s->raster_scratch,s->raster_status};
        for(unsigned i=0;i<5;i++){assert(get(handles[i]));for(unsigned k=0;k<i;k++)assert(handles[i]!=handles[k]);}
        assert(s->vertex_status==status_handle && !j->command_count);
        assert(s->setup.lanes<=4095 && s->setup.triangle_count==s->setup.lanes/3u);
        assert(s->setup.varying_count==2 && j->varying_count==2);
        assert(s->setup.flat_varying_mask==(g_gl.shade_model==GL_FLAT?1u:0u));
        assert(s->setup.viewport_x==g_gl.vp_x && s->setup.viewport_y==g_gl.vp_y);
        assert(s->setup.viewport_w==g_gl.vp_w && s->setup.viewport_h==g_gl.vp_h);
        kshs_storage_layout_t layout;assert(kshs_storage_layout(s->setup.triangle_count,&layout));
        assert(get(s->workspace)->bytes>=layout.bytes);
        assert(get(s->raster_scratch)->bytes>=KSHR_JOB_ALLOCATION_BYTES(j));
        assert(kshr_register_fast_eligible(j));
        const gl_texture_t *t=g_gl.bound_texture<GL_MAX_TEXTURES?&g_gl.textures[g_gl.bound_texture]:NULL;
        bool textured=g_gl.texture_2d && t && t->used && t->texels && t->width>0 && t->height>0;
        if(textured){
            memory *tex=get(r->source);assert(tex && j->texture_count==1);
            const ksh_texture_t *d=&j->textures[0];
            assert(d->width==(unsigned)t->width && d->height==(unsigned)t->height && d->pitch==d->width);
            assert(d->filter==(t->mag_filter==GL_NEAREST?0u:1u));
            assert(d->wrap_s==(t->wrap_s==GL_REPEAT?0u:1u) && d->wrap_t==(t->wrap_t==GL_REPEAT?0u:1u));
            uint64_t bytes=(uint64_t)t->width*t->height*4;
            assert(d->offset<=tex->bytes && bytes<=tex->bytes-d->offset);
            assert(!memcmp(tex->data+d->offset,t->texels,(size_t)bytes));
        }else assert(!r->source && !j->texture_count);
        fixed_last_fragment=*j;fixed_last_setup=s->setup;
        geometry_launches++;
        if(fixed_fault==6)return -EIO;
        unsigned emitted=0;
        if(!test_model_setup(s,(const float*)get(s->registers)->data,
            (const ksh_status_t*)get(s->vertex_status)->data,get(s->workspace)->data,
            observed+records,20000-records,&emitted))return -EIO;
        assert(fixed_observed_count+emitted<=5000);
        for(unsigned i=0;i<emitted;i++)fixed_observed[fixed_observed_count++]=observed[records+i].raster;
        fixed_draws+=emitted;records+=emitted;r->count=emitted;
        r->bytes=(uint64_t)emitted*j->code_count; // model metadata, never GPU timing
        return 0;
    }
'''

TESTS = r'''
bool reference_vertex;
static unsigned fixed_draws;
bool fixed_expect_resident;
static void fixed_triangle(void){
    fixed_expect_resident=true;
    glBegin(GL_TRIANGLES);glColor4f(.2f,.4f,.6f,.8f);glTexCoord2f(.25f,.75f);
    glVertex3f(-.5f,-.5f,0);glVertex3f(.5f,-.5f,0);glVertex3f(0,.5f,0);glEnd();
    fixed_expect_resident=false;assert(glGetError()==GL_NO_ERROR);
}
static void fixed_sample(int unit,float s,float t,float *out){
    assert(unit==0 && fabsf(s-.25f)<.00001f && fabsf(t-.75f)<.00001f);
    out[0]=.8f;out[1]=.3f;out[2]=.1f;out[3]=.25f;
}
static void fixed_fragment_result(unsigned env,bool textured){
    sh_shader_t shader={.compiled=true,.count=(int)fixed_last_fragment.code_count};
    memcpy(shader.code,fixed_last_fragment.code,sizeof shader.code);
    sh_machine_t m={0};m.sample=fixed_sample;
    memcpy(m.reg,fixed_last_fragment.seed,sizeof m.reg);
    const float colour[4]={.2f,.4f,.6f,.8f},texel[4]={.8f,.3f,.1f,.25f};
    memcpy(m.reg[SR_VARYING],colour,sizeof colour);
    m.reg[SR_VARYING+1][0]=.25f;m.reg[SR_VARYING+1][1]=.75f;
    ref_sh_run(&m,&shader);assert(!m.discarded && m.executed<=(int)fixed_last_fragment.budget);
    unsigned samples=0;for(unsigned i=0;i<fixed_last_fragment.code_count;i++)samples+=fixed_last_fragment.code[i].op==SH_TEX;
    assert(samples==(textured?1u:0u));
    for(unsigned k=0;k<4;k++){
        float expected=colour[k];
        if(textured){
            if(env==GL_REPLACE)expected=texel[k];
            else if(env==GL_DECAL){if(k<3)expected=colour[k]*(1-texel[3])+texel[k]*texel[3];}
            else expected*=texel[k];
        }
        assert(fabsf(m.reg[SR_FRAGCOLOR][k]-expected)<.00001f);
    }
}
static void fixed_resident_tests(void){
    unsigned reads=vertex_readbacks,draws=raster_launches,batches=geometry_launches,base=records;
    fixed_triangle();
    assert(geometry_launches==batches+1 && vertex_readbacks==reads && raster_launches==draws);
    assert(records==base+1);
    const kshr_command_t *c=&observed[base];
    for(unsigned v=0;v<3;v++){
        float iw=c->raster.v[v].inv_w;
        assert(fabsf(c->varying[v][0][0]/iw-.2f)<.00001f);
        assert(fabsf(c->varying[v][0][3]/iw-.8f)<.00001f);
        assert(fabsf(c->varying[v][1][0]/iw-.25f)<.00001f);
        assert(fabsf(c->varying[v][1][1]/iw-.75f)<.00001f);
    }
    glShadeModel(GL_FLAT);base=records;fixed_expect_resident=true;
    glBegin(GL_TRIANGLES);
    glColor3f(1,0,0);glTexCoord2f(0,0);glVertex3f(-.5f,-.5f,0);
    glColor3f(0,1,0);glTexCoord2f(1,0);glVertex3f(.5f,-.5f,0);
    glColor3f(0,0,1);glTexCoord2f(.5f,1);glVertex3f(0,2,0);glEnd();
    fixed_expect_resident=false;assert(glGetError()==GL_NO_ERROR && records>base);
    bool varying_uv=false;
    for(unsigned i=base;i<records;i++)for(unsigned v=0;v<3;v++){
        const kshr_command_t *p=&observed[i];float iw=p->raster.v[v].inv_w;
        assert(p->varying[v][0][0]==0 && p->varying[v][0][1]==0 && fabsf(p->varying[v][0][2]/iw-1)<.00001f);
        varying_uv|=fabsf(p->varying[v][1][0]/iw-.5f)>.001f;
    }
    assert(varying_uv);glShadeModel(GL_SMOOTH);
    GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glEnable(GL_TEXTURE_2D);
    const unsigned modes[3]={GL_REPLACE,GL_MODULATE,GL_DECAL};
    for(unsigned i=0;i<3;i++){
        glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,modes[i]);fixed_triangle();fixed_fragment_result(modes[i],false);
    }
    const unsigned char rgba[16]={255,0,0,64,0,255,0,128,0,0,255,192,255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    unsigned initial_uploads=texture_uploads;
    for(unsigned i=0;i<3;i++){
        glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,modes[i]);fixed_triangle();fixed_fragment_result(modes[i],true);
        if(!i)assert(texture_uploads==initial_uploads+1);
        unsigned uploads=texture_uploads;fixed_triangle();assert(texture_uploads==uploads);
    }
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    unsigned sampler_uploads=texture_uploads;
    fixed_triangle();assert(fixed_last_fragment.textures[0].wrap_s==1 && fixed_last_fragment.textures[0].filter==0);
    assert(texture_uploads==sampler_uploads);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    fixed_triangle();assert(texture_uploads==sampler_uploads+1 && fixed_last_fragment.textures[0].width==1);
    glDeleteTextures(1,&texture);fixed_triangle();fixed_fragment_result(GL_DECAL,false);
    glDisable(GL_TEXTURE_2D);
    assert(vertex_readbacks==reads && raster_launches==draws);
    puts("PASS modeled fixed op9: no CPU clipping/register readback; colour/UV, clipped flat colour, texture descriptors/cache and environment alpha semantics");
}
static void compare_vertex(const gl_vertex_t *a,const gl_vertex_t *b){
    const float pa[10]={a->clip.x,a->clip.y,a->clip.z,a->clip.w,a->r,a->g,a->b,a->a,a->s,a->t};
    const float pb[10]={b->clip.x,b->clip.y,b->clip.z,b->clip.w,b->r,b->g,b->b,b->a,b->s,b->t};
    for(unsigned k=0;k<10;k++){
        float tolerance=0.00004f*(1.f+fabsf(pb[k]));
        if(!(fabsf(pa[k]-pb[k])<=tolerance))printf("vertex field %u: GPU %.9g CPU %.9g\n",k,pa[k],pb[k]);
        assert(fabsf(pa[k]-pb[k])<=tolerance);
    }
}
static void fixed_tests(void){
#ifdef FIXED_CUDA
    surface_t *owned=surface_create(128,128);assert(gui_gpu_attach(owned));
    surface_t canvas=*owned;
#else
    struct gui_gpu_surface native={0};
    surface_t canvas={.width=128,.height=128,.stride=128,.gpu=&native};
#endif
    glSetTarget(&canvas);glViewport(0,0,128,128);
    static gl_vertex_t captured[513],reference[513];
    for(unsigned variant=0;variant<32;variant++){
        glMatrixMode(GL_MODELVIEW);glLoadIdentity();
        glTranslatef(.1f,-.2f,-2);glRotatef(23,.2f,.7f,.1f);glScalef(.4f,1.7f,.8f);
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glFrustum(-1,1,-1,1,.1,100);
        g_gl.lighting=!!(variant&1);g_gl.colour_material=!!(variant&2);g_gl.normalize=!!(variant&4);
        for(unsigned l=0;l<GL_MAX_LIGHTS;l++){
            gl_light_t *light=&g_gl.lights[l];light->enabled=l==0 || !!(variant&8);
            light->position[0]=l*.3f;light->position[1]=.4f;light->position[2]=.8f;
            light->position[3]=(l&1)?1:0;
            light->attenuation[0]=.8f;light->attenuation[1]=.2f;light->attenuation[2]=.07f;
            for(unsigned k=0;k<3;k++){
                light->ambient[k]=.02f*(l+1);light->diffuse[k]=.1f*(k+1);light->specular[k]=.1f;
            }
        }
        for(unsigned i=0;i<513;i++){
            float x=(int)(i%17)-8,y=(int)(i%13)-6,z=(int)(i%7)-3;
            glColor4f(i*.001f,.3f,.5f,.4f);
            // Raw normal captures; normalization happens in the GPU stage.
            g_gl.cur_normal[0]=(i%9)*.2f;g_gl.cur_normal[1]=.1f;g_gl.cur_normal[2]=.7f;
            if(i%31==0)memset(g_gl.cur_normal,0,sizeof g_gl.cur_normal);
            glTexCoord2f(i*.01f,-.4f);
            for(unsigned k=0;k<4;k++){
                g_gl.material.ambient[k]=.05f*(k+1);g_gl.material.diffuse[k]=.12f*(k+1);
                g_gl.material.specular[k]=.3f;g_gl.material.emission[k]=i*.0001f;
            }
            g_gl.material.shininess=(variant&16)?16:0;
            reference_vertex=true;gl_process_vertex(x,y,z,1,&reference[i]);reference_vertex=false;
            gl_fixed_capture(x,y,z,1,&captured[i]);assert(captured[i].fixed_pending);
        }
        // Later material/current-attribute changes must not recolour queued input.
        memset(&g_gl.material,0,sizeof g_gl.material);glColor4f(99,99,99,99);
        assert(gl_fixed_transform_gpu(captured,513));
        for(unsigned i=0;i<513;i++){assert(!captured[i].fixed_pending);compare_vertex(&captured[i],&reference[i]);}
        unsigned before=vertex_launches;assert(gl_fixed_transform_gpu(captured,513));assert(vertex_launches==before);
        // Carried strip/fan vertices have already been transformed: skip them.
        captured[17]=reference[17];gl_fixed_capture(.3f,.4f,.5f,1,&captured[511]);
        reference_vertex=true;gl_process_vertex(.3f,.4f,.5f,1,&reference[511]);reference_vertex=false;
        assert(gl_fixed_transform_gpu(captured,513));compare_vertex(&captured[17],&reference[17]);compare_vertex(&captured[511],&reference[511]);
    }
    g_gl.lighting=g_gl.normalize=g_gl.colour_material=false;
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
#ifdef FIXED_CUDA
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
#endif
    unsigned before=vertex_launches;
    unsigned first_reads=vertex_readbacks,first_geometries=geometry_launches;
    fixed_expect_resident=true;
    glBegin(GL_TRIANGLES);glColor3f(1,0,0);glVertex3f(-.5f,-.5f,0);
    glColor3f(0,1,0);glVertex3f(.5f,-.5f,0);glColor3f(0,0,1);glVertex3f(0,.5f,0);glEnd();
    fixed_expect_resident=false;
    assert(glGetError()==GL_NO_ERROR && vertex_launches==before+1 && fixed_draws && !canvas.pixels);
    assert(vertex_readbacks==first_reads && geometry_launches==first_geometries+1);
#ifdef FIXED_CUDA
    colour_t pixels[128*128];assert(gui_gpu_colour_readback(&canvas,pixels,128,128*128));
    colour_t c=pixels[64*128+64];
    assert(abs((int)RGB_R(c)-63)<=4 && abs((int)RGB_G(c)-67)<=4 && abs((int)RGB_B(c)-126)<=4);
#endif
#ifndef FIXED_CUDA
    const unsigned expected[10]={4101,2050,4101,4100,1367,4099,4099,2050,4098,4099};
    for(GLenum mode=GL_POINTS;mode<=GL_POLYGON;mode++){
        unsigned start_draws=fixed_draws;fixed_observed_count=0;
        unsigned start_reads=vertex_readbacks,start_geometry=geometry_launches;
        fixed_expect_resident=mode==GL_TRIANGLES;
        glBegin(mode);
        for(unsigned i=0;i<4101;i++){
            float x,y;
            if(mode==GL_TRIANGLES){unsigned k=i%3;x=k==1?.7f:k==0?-.7f:0;y=k==2?.7f:-.7f;}
            else if(mode==GL_TRIANGLE_FAN||mode==GL_POLYGON){
                unsigned k=(i-1)&3;x=(k==0||k==3)?-.7f:.7f;y=k<2?-.7f:.7f;
                if(!i)x=y=0;
            }else{unsigned k=i&3;x=k<2?-.7f:.7f;y=(k&1)?.7f:-.7f;}
            glColor3f(i/5000.f,.4f,.2f);glVertex3f(x,y,0);
        }
        glEnd();fixed_expect_resident=false;assert(glGetError()==GL_NO_ERROR);
        if(mode==GL_TRIANGLES)assert(vertex_readbacks==start_reads && geometry_launches==start_geometry+2);
        if(fixed_draws-start_draws!=expected[mode])printf("mode %u records %u expected %u\n",mode,fixed_draws-start_draws,expected[mode]);
        assert(fixed_draws-start_draws==expected[mode]);
        if(mode==GL_LINE_LOOP){
            const kg3d_command_t *last=&fixed_observed[fixed_observed_count-1];
            assert(fabsf(last->v[0].rgba[0]-4100/5000.f)<.00001f && last->v[1].rgba[0]==0);
        }
    }
    puts("PASS fixed primitive capacity: all ten modes retain 4101-vertex continuity; line loop closes only at End");
    fixed_observed_count=0;fixed_resident_tests();
#endif
    glBegin(GL_POINTS);
    mat4_t saved=g_gl.modelview[g_gl.mv_top];
    glLoadIdentity();assert(glGetError()==GL_INVALID_OPERATION);
    glTranslatef(1,2,3);assert(glGetError()==GL_INVALID_OPERATION);
    glPushMatrix();assert(glGetError()==GL_INVALID_OPERATION);
    float light[4]={1,2,3,1};glLightfv(GL_LIGHT0,GL_POSITION,light);assert(glGetError()==GL_INVALID_OPERATION);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT,light);assert(glGetError()==GL_INVALID_OPERATION);
    glDrawArrays(GL_TRIANGLES,0,3);assert(glGetError()==GL_INVALID_OPERATION && g_gl.in_begin);
    glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_INT,(const void*)1);assert(glGetError()==GL_INVALID_OPERATION && g_gl.in_begin);
    assert(!memcmp(&saved,&g_gl.modelview[g_gl.mv_top],sizeof saved));glEnd();
    assert(glGetError()==GL_NO_ERROR);
    glSetTarget(NULL);
#ifdef FIXED_CUDA
    surface_destroy(owned);
#else
    assert(creates==destroys);
#endif
    printf("PASS fixed vertex arithmetic/material snapshots: 16416 reference vertices, 32 lighting states, mixed retired/pending vertices and real GL draw routing; CPU transforms only in reference\n");
}
#ifndef FIXED_CUDA
static void failure_test(int fault){
    struct gui_gpu_surface native={0};surface_t canvas={.width=128,.height=128,.stride=128,.gpu=&native};
    glSetTarget(&canvas);glViewport(0,0,128,128);fixed_fault=fault;
    glBegin(fault==6?GL_TRIANGLES:GL_POINTS);
    // Cross the old immediate capacity. An early failed flush must stop capture,
    // not continue into strip carry logic or render a partial transformed batch.
    for(unsigned i=0;i<5200;i++){
        if(fault==6){unsigned k=i%3;glVertex3f(k==0?-.5f:k==1?.5f:0,k==2?.5f:-.5f,0);}
        else glVertex3f(.1f,.2f,.3f);
    }
    glEnd();assert(glGetError()==GL_INVALID_OPERATION && native.invalid && !canvas.pixels);
    assert(!fixed_draws && g_gl.gpu_fixed_failed && !g_gl.in_begin);
    assert(vertex_launches==(fault==5?0u:1u));
    if(fault==6)assert(geometry_launches==1 && !vertex_readbacks && !raster_launches);
    fixed_fault=0;unsigned before=requests;glSetTarget(NULL);
    if(fault==1 || fault==2 || fault==6)assert(requests==before && creates>destroys);
    else assert(creates==destroys);
    printf("PASS fixed vertex failure %d: no partial draw/CPU fallback; appropriate fenced release or unfenced quarantine\n",fault);
}
#endif
int main(int argc,char **argv){setbuf(stdout,NULL);
#ifndef FIXED_CUDA
    if(argc>1){failure_test(atoi(argv[1]));return 0;}
#else
    (void)argc;(void)argv;
#endif
    fixed_tests();return 0;}
'''

def run():
    header=HEADER
    for p in ['include/kestrel/gpu2d.h','include/kestrel/gpu3d.h','include/kestrel/shader_vm.h',
              'include/kestrel/shader_raster.h','include/kestrel/shader_setup.h','user/libgl/GL.h','user/libgl/shader.h',
              'user/libgl/glstate.h','user/libgl/gpuvm.h']:
        header+='\n'+strip_includes((ROOT/p).read_text())
    header+='\nextern bool reference_vertex,fixed_expect_resident;\nvoid ref_sh_run(sh_machine_t*,const sh_shader_t*);\n'
    header+='bool gui_gpu_draw3d(surface_t*,const surface_t*,const kg3d_command_t*,unsigned,rect_t);\n#endif\n'
    host=HARNESS[:HARNESS.index('static GLuint shader(')]
    host=host.replace('    (void)s;(void)t;(void)c;(void)n;(void)r;assert(false);return false;',
        '    assert(s->gpu&&!s->pixels&&!t&&n&&c);(void)r;fixed_draws+=n;'
        'assert(fixed_observed_count+n<=5000);memcpy(fixed_observed+fixed_observed_count,c,n*sizeof(*c));fixed_observed_count+=n;return true;')
    host=host.replace('#include "test.h"','#include "test.h"\nstatic unsigned fixed_draws;\nstatic int fixed_fault;'
        '\nstatic kg3d_command_t fixed_observed[5000];\nstatic unsigned fixed_observed_count;'
        '\nstatic kshr_job_t fixed_last_fragment;\nstatic kshs_config_t fixed_last_setup;')
    geometry_start=host.index('    if(r->operation==KG2D_SHADER_GEOMETRY){')
    geometry_end=host.index('    assert(r->operation==KG2D_SHADER_RASTER',geometry_start)
    host=host[:geometry_start]+FIXED_GEOMETRY+host[geometry_end:]
    host=host.replace('    kg2d_request_t *r=(void*)ptr;', '''    kg2d_request_t *r=(void*)ptr;
    if(fixed_fault==1 && r->operation==KG2D_SHADER_VM){vertex_launches++;return -EIO;}
    if(fixed_fault==2 && r->operation==KG2D_DOWNLOAD && r->offset)return -EIO;
    if(fixed_fault==5 && r->operation==KG2D_UPLOAD)return -EBUSY;''')
    host=host.replace('        }in_model=false;return 0;', '''        }in_model=false;
        if(fixed_fault==3)((ksh_status_t*)status->data)[j->lanes-1].result=KSH_PENDING;
        if(fixed_fault==4)((ksh_status_t*)status->data)[j->lanes-1].result=KSH_DISCARDED;
        return 0;''')
    tests=TESTS.replace('static unsigned fixed_draws;','')
    clang=shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    with tempfile.TemporaryDirectory(prefix='kestrel-fixed-vertex-') as tmp:
        tmp=Path(tmp);(tmp/'test.h').write_text(header);files=[]
        for name in ['gl.c','glmath.c','gltex.c','raster.c','gpu.c','shaderapi.c',
                     'shadergpu.c','fixedgpu.c','gpuvm.c','glsl.c','shadervm.c']:
            src=strip_includes((ROOT/'user/libgl'/name).read_text())
            if name=='gl.c':
                anchor='void gl_process_vertex(float x, float y, float z, float w, gl_vertex_t *out) {'
                assert src.count(anchor)==1
                src=src.replace(anchor,anchor+'\n    assert(reference_vertex);')
            if name=='raster.c':
                anchor='    if (!g_gl.colour) return;'
                assert anchor in src
                src=src.replace(anchor,'    assert(!fixed_expect_resident);\n'+anchor,1)
            if name=='shadervm.c':src=src.replace('void sh_run(', 'void ref_sh_run(')
            if name=='gpuvm.c':
                anchor='    unsigned char *p=data;'
                assert src.count(anchor)==1
                src=src.replace(anchor,'    test_vm_transfer_role(buffer==&vm->textures);\n'+anchor)
            path=tmp/name;path.write_text('#include "test.h"\n'+src);files.append(str(path))
        (tmp/'test.c').write_text(host+tests);exe=tmp/'test.exe'
        model=tmp/'setup_model.obj'
        subprocess.run([clang,'-std=c++17','-O1','-ffp-contract=off','-c',str(ROOT/'tools/shader_setup_host_model.cpp'),'-o',str(model)],check=True)
        subprocess.run([clang,'-std=c11','-O1','-Wall','-Wextra',*files,str(tmp/'test.c'),str(model),'-o',str(exe)],check=True)
        import sys
        if '--compile-only' in sys.argv[1:]:
            print('COMPILED ONLY fixed GPU model: executable was not run')
            return
        subprocess.run([str(exe)],check=True)
        for fault in range(1,7):subprocess.run([str(exe),str(fault)],check=True)

if __name__=='__main__':
    run()
