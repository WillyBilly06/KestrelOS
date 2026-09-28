#!/usr/bin/env python3
"""Run production gfxtest shader checks through real GL code and sm_120 kernels.

Only the OS allocation/submission and GUI target boundary are replaced by CUDA.
CPU shader execution/demotion aborts. Does not prove native Kestrel QMD setup.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time
from nvshader import find_cl, tool
from test_gl_shader_gpu import HEADER, ROOT
from test_gl_gpu import strip_includes


class CudaTemporaryDirectory(tempfile.TemporaryDirectory):
    """A lingering compiler/scanner directory handle is not a GPU verdict.

    Retry only permission/sharing failures on this harness-owned directory.
    Retention is explicit; compile/test exceptions are never intercepted.
    """
    def cleanup(self):
        for attempt in range(10):
            try:
                super().cleanup()
                return
            except PermissionError:
                if attempt < 9:
                    time.sleep(0.1)
        print(f'WARNING: Windows still holds temporary directory {self.name}; '
              'cleanup deferred, test results unchanged.', file=sys.stderr)

HOST = r'''
#include "test.h"
intptr_t test_cuda_request(kg2d_request_t *);
void test_cuda_finished(void);
void test_cuda_failed(int);
static unsigned snapshots;
static int fault;
static bool request(kg2d_request_t *r){r->version=KG2D_ABI;return test_cuda_request(r)==0;}
intptr_t syscall6(intptr_t nr,intptr_t op,intptr_t ptr,intptr_t bytes,intptr_t a,intptr_t b,intptr_t c){
    assert(nr==SYS_GPU&&op==GPUOP_SURFACE&&bytes==sizeof(kg2d_request_t)&&!a&&!b&&!c);
    if(fault==3 && ((kg2d_request_t*)ptr)->operation==KG2D_SHADER_VM)return -EIO;
    if(((kg2d_request_t*)ptr)->operation==KG2D_SHADER_GEOMETRY){
        fputs("UNSUPPORTED: CUDA harness backend has no op9 setup/compact/resident-raster integration; this run cannot validate the current triangle-array path. No CPU fallback.\n",stderr);
        exit(2); // explicit incomplete harness, never a false GPU pass
    }
    return test_cuda_request((kg2d_request_t*)ptr);
}
rect_t rect_intersection(rect_t a,rect_t b){int x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int r=a.x+a.w<b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h<b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r>x?r-x:0,d>y?d-y:0);}
rect_t rect_union(rect_t a,rect_t b){int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
    int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w,d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
    return rect_make(x,y,r-x,d-y);}
bool rect_empty(rect_t r){return r.w<=0||r.h<=0;}
rect_t surface_clip(const surface_t *s){return rect_make(0,0,s->width,s->height);}
bool gui_gpu_flush(void){return true;}
surface_t *surface_create(int w,int h){
    surface_t *s=calloc(1,sizeof(*s));assert(s);s->width=w;s->height=h;s->stride=w;
    s->owns_pixels=true;s->pixels=calloc((size_t)w*h,4);assert(s->pixels);return s;
}
bool gui_gpu_attach(surface_t *s){
    if(fault==1)return false;
    assert(!s->gpu&&s->owns_pixels);s->gpu=calloc(1,sizeof(*s->gpu));assert(s->gpu);
    kg2d_request_t r={.operation=KG2D_CREATE,.width=s->width,.height=s->height};assert(request(&r));
    s->gpu->handle=r.handle;s->gpu->pitch=r.pitch;free(s->pixels);s->pixels=NULL;return true;
}
bool gui_gpu_target(surface_t *s,bool depth,gui_gpu_target_t *out){
    assert(s&&s->gpu&&!s->pixels&&!s->gpu->invalid);
    if(depth&&!s->gpu->depth){kg2d_request_t r={.operation=KG2D_CREATE,.width=s->width,.height=s->height};
        assert(request(&r));s->gpu->depth=r.handle;s->gpu->depth_pitch=r.pitch;}
    *out=(gui_gpu_target_t){s->gpu->handle,s->gpu->depth,s->gpu->pitch,s->gpu->depth_pitch};return true;
}
void gui_gpu_invalidate(surface_t *s){s->gpu->invalid=true;}
bool gui_gpu_cpu_access(surface_t *s){(void)s;assert(false);return false;}
bool gui_gpu_depth_readback(const surface_t *s,float *p,unsigned stride){(void)s;(void)p;(void)stride;assert(false);return false;}
bool gui_gpu_draw3d(surface_t *s,const surface_t *t,const kg3d_command_t *c,unsigned n,rect_t bounds){
    assert(!t);bool depth=false;for(unsigned i=0;i<n;i++)depth|=!!(c[i].flags&KG3D_CLEAR_DEPTH);
    gui_gpu_target_t target;assert(gui_gpu_target(s,depth,&target));
    kg2d_request_t r={.operation=KG2D_DRAW3D,.handle=target.colour,.offset=target.depth,
        .data=(uintptr_t)c,.count=n,.x=bounds.x,.y=bounds.y,.width=bounds.w,.height=bounds.h};return request(&r);
}
bool gui_gpu_colour_readback(const surface_t *s,colour_t *out,unsigned stride,size_t capacity){
    assert(s->gpu&&!s->pixels&&stride>=(unsigned)s->width&&capacity>=(size_t)(s->height-1)*stride+s->width);
    if(s->gpu->invalid)return false;
    if(fault==2)return false;
    for(int y=0;y<s->height;y++){
        kg2d_request_t r={.operation=KG2D_DOWNLOAD,.handle=s->gpu->handle,
            .offset=(uint64_t)y*s->gpu->pitch,.bytes=(uint64_t)s->width*4,.data=(uintptr_t)(out+(size_t)y*stride)};
        assert(request(&r));
    }snapshots++;return true;
}
surface_t *gui_gpu_image(const colour_t *p,int w,int h){(void)p;(void)w;(void)h;assert(false);return NULL;}
void surface_destroy(surface_t *s){
    if(!s)return;
    if(s->gpu){
        if(s->gpu->depth){kg2d_request_t d={.operation=KG2D_DESTROY,.handle=s->gpu->depth};assert(request(&d));}
        kg2d_request_t c={.operation=KG2D_DESTROY,.handle=s->gpu->handle};assert(request(&c));free(s->gpu);
    }free(s->pixels);free(s);
}
void log_write(int level,const char *tag,const char *note){(void)level;(void)tag;puts(note);}
void sh_run(sh_machine_t *m,const sh_shader_t *s){(void)m;(void)s;assert(false);}
'''

if __name__ == '__main__':
    desktop_scene = sys.argv[1:] == ['--desktop-scene']
    fixed_vertex = sys.argv[1:] == ['--fixed-vertex']
    if sys.argv[1:] and not desktop_scene and not fixed_vertex:
        raise SystemExit('usage: test_gl_shader_cuda.py [--desktop-scene|--fixed-vertex]')
    header = HEADER.replace('struct gui_gpu_surface {unsigned id;bool invalid;};',
        'struct gui_gpu_surface {uint64_t handle,depth;unsigned pitch,depth_pitch;bool invalid;};')
    for p in ['include/kestrel/gpu2d.h', 'include/kestrel/gpu3d.h', 'include/kestrel/shader_vm.h',
              'include/kestrel/shader_raster.h', 'include/kestrel/shader_setup.h', 'user/libgl/GL.h', 'user/libgl/shader.h',
              'user/libgl/glstate.h', 'user/libgl/gpuvm.h']:
        header += '\n' + strip_includes((ROOT/p).read_text())
    header += '\nbool gui_gpu_draw3d(surface_t*,const surface_t*,const kg3d_command_t*,unsigned,rect_t);\n#endif\n'
    tests = (ROOT/'user/gfxtest/gfxtest.c').read_text()
    tests = tests[tests.index('static bool shader_ok('):tests.index('int main(')]
    main = r'''
int main(int argc,char **argv){
    fault=argc>1?atoi(argv[1]):0;
    assert(fault>=0&&fault<=3);
    int result=shader_tests(true);
    if(fault){
        assert(result!=0&&!snapshots);test_cuda_failed(fault==3);
        printf("PASS negative gfxtest gate %d: failure reported, no pixel PASS or CPU fallback\n",fault);
        return 0;
    }
    assert(!result&&snapshots==12);test_cuda_finished();return result;
}
'''
    host = HOST
    if fixed_vertex:
        from test_gl_fixed_gpu import TESTS
        header += '\n#define FIXED_CUDA 1\nextern bool reference_vertex;\n'
        host = host.replace('#include "test.h"', '#include "test.h"\nstatic unsigned fixed_draws;\n'
            'void test_cuda_counts(unsigned out[3]);\nvoid test_cuda_fixed_finished(void);\n'
            'static unsigned fixed_vertex_count(void){unsigned c[3];test_cuda_counts(c);return c[0];}\n'
            '#define vertex_launches fixed_vertex_count()')
        host = host.replace('    assert(!t);bool depth=false;', '    fixed_draws+=n;assert(!t);bool depth=false;')
        fixed_tests = TESTS.replace('static unsigned fixed_draws;', '')
        fixed_tests = fixed_tests.replace('fixed_tests();return 0;',
            'fixed_tests();assert(!fixed_vertex_tests());test_cuda_fixed_finished();return 0;')
        tests += fixed_tests
        main = ''
    if desktop_scene:
        header += '\n#define M_TAU 6.28318530717958647692\n'
        header += 'void unused_cpu_sh_run(sh_machine_t*,const sh_shader_t*);\n'
        host = host.replace('void sh_run(sh_machine_t *m,const sh_shader_t *s){(void)m;(void)s;assert(false);}',
            'static bool scene_reference;\nvoid sh_run(sh_machine_t *m,const sh_shader_t *s){assert(scene_reference);unused_cpu_sh_run(m,s);}')
        tests = strip_includes((ROOT/'user/desktop/app_gl_scene.h').read_text())
        tests += '\n' + strip_includes((ROOT/'user/desktop/app_gl_scene.c').read_text())
        # Compare the specialized GPU ground against the ORIGINAL general
        # material on the CPU oracle, not the same new shader on both sides.
        anchor = '        glUseProgram(s->ground_program);'
        tail = '        ok=scene_status("ground draw");'
        assert tests.count(anchor)==1 and tests.count(tail)==1
        tests = tests.replace(anchor,
            '        if(scene_reference){ok=scene_draw(s,s->ground,64*6,false,false);}else{\n'+anchor)
        tests = tests.replace(tail,tail+'\n        }')
        main = (ROOT/'tools/test_desktop_gl_scene.c').read_text()
    env = dict(os.environ)
    cl = find_cl()
    if cl:
        env['PATH'] = cl + os.pathsep + env.get('PATH', '')
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    with CudaTemporaryDirectory(prefix='kestrel-gl-cuda-') as tmp:
        tmp = Path(tmp)
        (tmp/'test.h').write_text(header)
        files = []
        for name in ['gl.c', 'glmath.c', 'gltex.c', 'raster.c', 'gpu.c', 'shaderapi.c',
                     'shadergpu.c', 'fixedgpu.c', 'gpuvm.c', 'glsl.c', 'shadervm.c']:
            src = strip_includes((ROOT/'user/libgl'/name).read_text())
            if fixed_vertex and name == 'gl.c':
                anchor='void gl_process_vertex(float x, float y, float z, float w, gl_vertex_t *out) {'
                assert src.count(anchor)==1
                src=src.replace(anchor,anchor+'\n    assert(reference_vertex);')
            if desktop_scene and name == 'raster.c':
                # Match the native raster contract in the REFERENCE ONLY. The
                # legacy software path uses near-only clipping and unsnapped
                # floating edges; the GPU deliberately uses full clipping and
                # a watertight 1/16-pixel screen grid. A checker boundary can
                # therefore differ by a whole row without any shader error.
                old = 'g_gl.colour->gpu ? clip_gpu(in,clipped) : clip_near(in, 3, clipped)'
                assert src.count(old) == 1
                src = src.replace(old, 'clip_gpu(in,clipped)')
                anchor = '    float area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);'
                assert src.count(anchor) == 1
                quantize = '''    frag_vertex_t snapped[3]={*v0,*v1,*v2};
    for(unsigned i=0;i<3;i++){
        snapped[i].x=nearbyintf(snapped[i].x*16)/16;
        snapped[i].y=nearbyintf(snapped[i].y*16)/16;
    }
    v0=&snapped[0];v1=&snapped[1];v2=&snapped[2];
'''
                src = src.replace(anchor, quantize+anchor)
            if name == 'shadervm.c':
                src = src.replace('void sh_run(', 'void unused_cpu_sh_run(')
            path = tmp/name
            path.write_text('#include "test.h"\n'+src)
            files.append(path)
        (tmp/'test.c').write_text(host + tests + main)
        files.append(tmp/'test.c')
        objects = []
        for path in files:
            obj = path.with_suffix('.obj')
            subprocess.run([clang, '-std=c11', '-O1', '-Wall', '-Wextra', '-c', str(path), '-o', str(obj)], check=True)
            objects.append(str(obj))
        for name in ['shader_vm', 'shader_raster', 'shader_raster_fast', 'gl_raster', 'test_gl_shader_cuda']:
            obj = tmp/(name+'_cuda.obj')
            subprocess.run([tool('nvcc'), '-arch=sm_120', '-O2', '-c',
                str(ROOT/'tools'/f'{name}.cu'), '-o', str(obj)], check=True, env=env, cwd=tmp, timeout=120)
            objects.append(str(obj))
        exe = tmp/'test.exe'
        subprocess.run([tool('nvcc'), *objects, '-o', str(exe)], check=True, env=env, cwd=tmp, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=120)
        for fault in (() if desktop_scene or fixed_vertex else ('1', '2', '3')):
            subprocess.run([str(exe), fault], check=True, timeout=120)
