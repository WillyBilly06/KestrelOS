#!/usr/bin/env python3
"""CPU-only tests of production adapter dispatch and NVIDIA ownership checks.

No GPU access, shader/codec simulation, or VM. Driver operations are call spies;
these tests verify routing and refusal, not hardware rendering.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_gpu_variant_safety import function

ROOT = Path(__file__).resolve().parents[1]


def main():
    gpu = (ROOT / "kernel/gpu.c").read_text()
    header = (ROOT / "kernel/gpu.h").read_text()
    adapter = (ROOT / "kernel/nv_accel.c").read_text()
    channel = (ROOT / "kernel/nv_chan.c").read_text()
    client = (ROOT / "kernel/nvkms_kapi_client.c").read_text()
    console = (ROOT / "kernel/console.c").read_text()
    svga = (ROOT / "kernel/svga.c").read_text()
    syscall = (ROOT / "kernel/syscall.c").read_text()
    draw_arms = syscall[syscall.index('        case GPUOP_CANDRAW:'):syscall.index('        case GPUOP_SHADERS:')]
    for name in ('gpu_accel_draw_mode', 'gpu_accel_read_pixel',
                 'gpu_triangle_request', 'gpu_accel_draw_image'):
        assert name + '(' in draw_arms
    assert 'nvkms_kapi_runtime_' not in draw_arms and 'svga3d_' not in draw_arms
    shader_arms = syscall[syscall.index('        case GPUOP_SHADERS:'):syscall.index('        case GPUOP_START:')]
    assert 'gpu_shaders_request(a1)' in shader_arms and 'gpu_layout_request(a1)' in shader_arms
    assert 'svga3d_' not in shader_arms
    publish = function(client, "nvkms_kapi_publish_runtime_framebuffer")
    assert publish.index("nv_chan_display_card()") < publish.index("gpu_accel_select(") < publish.index("console_publish_framebuffer(")
    assert publish.index("render_card != test_card") < publish.index("dma_alloc_pages(")
    assert "gpu_accel_clear();" in function(console, "console_abandon_framebuffer")
    assert "gpu_accel_select(svga.pci->bus" in function(svga, "svga_adopt_framebuffer")
    # Compile the actual complete dispatch implementation and ops definition.
    start = gpu.index("typedef struct {\n    u8 bus, slot, func;")
    end = gpu.index("/* The GPU half of the shutdown verdict", start)
    ops = re.search(r"typedef struct \{\s*bool \(\*can_fill\)[\s\S]+?\} gpu_accel_ops_t;", header)[0]
    source = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define MAX_GPUS 4
#define ARRAY_LEN(a) (sizeof(a)/sizeof((a)[0]))
#define GPU_ACCEL_CAN_FILL 1
#define GPU_ACCEL_CAN_COPY 2
#define GPU_ACCEL_CAN_CURSOR 4
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
'''
    source += re.search(r'^#define GPU_PROGRAM_SVGA_DX\s+[^\n]+', header, re.M)[0]+'\n'
    source += re.search(r'typedef struct \{[^{}]*\} gpu_shader_program_t;', header)[0]+'\n'
    source += ops + "\n" + gpu[start:end]
    source += r'''
typedef struct { u8 pci_bus, pci_slot, pci_func; } nv_card_t;
typedef struct { int unused; } nv_fifo_t;
enum { CH_COPY, CH_GFX };
typedef struct { bool open, submit_failed; nv_card_t *card; } nv_channel_t;
static nv_channel_t channels[2];
static bool g_scanout_bound, g_compute_scanout_bound;
'''
    source += function(channel, "nv_chan_display_ready") + "\n"
    source += function(channel, "nv_chan_display_card") + "\n"
    source += function(channel, "nv_chan_raster_ready") + "\n"
    source += r'''
static bool runtime_ready;
static nv_card_t *test_card;
static unsigned nv_fill_calls, nv_copy_calls, kapi_fill_calls, kapi_copy_calls;
static unsigned kapi_draw_calls,kapi_image_calls,kapi_pixel_calls;
static const float *last_vertices;
static const u32 *last_pixels;
static u32 last_triangles,last_width,last_height,last_stride,last_draw_width,last_draw_height;
static int last_x,last_y;
static bool draw_success=true;
static bool nvkms_kapi_runtime_ready(void) { return runtime_ready; }
static int nvkms_kapi_runtime_draw(const float *v,u32 n) {
    kapi_draw_calls++;last_vertices=v;last_triangles=n;return draw_success?(int)n:-1;
}
static bool nvkms_kapi_runtime_present(const u32 *p,u32 w,u32 h,u32 stride,int x,int y) {
    kapi_image_calls++;last_pixels=p;last_width=w;last_height=h;last_stride=stride;
    last_x=x;last_y=y;return draw_success;
}
static bool nvkms_kapi_runtime_read_pixel(int x,int y,u32 *p) {
    kapi_pixel_calls++;last_x=x;last_y=y;*p=0xaabbccdd;return draw_success;
}
static bool nv_chan_fill_scanout(int x,int y,int w,int h,u32 c) {
    (void)x;(void)y;(void)w;(void)h;(void)c;nv_fill_calls++;return true;
}
static bool nv_chan_copy_scanout(int sx,int sy,int dx,int dy,int w,int h) {
    (void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;nv_copy_calls++;return true;
}
static bool nvkms_kapi_runtime_fill(int x,int y,int w,int h,u32 c) {
    (void)x;(void)y;(void)w;(void)h;(void)c;kapi_fill_calls++;return true;
}
static bool nvkms_kapi_runtime_copy(int sx,int sy,int dx,int dy,int w,int h) {
    (void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;kapi_copy_calls++;return true;
}
'''
    source += function(client, "nvkms_kapi_runtime_matches_gpu") + "\n"
    source += function(client, "nvkms_kapi_runtime_selected") + "\n"
    source += re.sub(r'^#include[^\n]*', '', adapter, flags=re.M)
    source += r'''
static int svga;
static bool svga_up=true;
static unsigned svga_draw_calls,svga_image_calls;
static unsigned shader_calls,layout_calls;
static gpu_shader_program_t last_vs,last_ps;
static const u32 *last_elements;
static u32 last_count;
static bool svga3d_can_draw(void) { return svga_up; }
static int svga3d_draw_user(const float *v,u32 n) {
    svga_draw_calls++;last_vertices=v;last_triangles=n;return draw_success?(int)n:-1;
}
static int svga3d_draw_image_strided(const u32 *p,u32 w,u32 h,u32 stride,int x,int y,u32 dw,u32 dh) {
    svga_image_calls++;last_pixels=p;last_width=w;last_height=h;last_stride=stride;
    last_x=x;last_y=y;last_draw_width=dw;last_draw_height=dh;return draw_success?0:-1;
}
static int svga3d_set_shaders(const u32 *vs,u32 vw,const u32 *ps,u32 pw,
    const u32 *vi,u32 vin,const u32 *vo,u32 von,const u32 *pi,u32 pin,const u32 *po,u32 pon) {
    shader_calls++;last_vs=(gpu_shader_program_t){vs,vi,vo,vw,vin,von};
    last_ps=(gpu_shader_program_t){ps,pi,po,pw,pin,pon};return draw_success?0:-1;
}
static int svga3d_set_layout(const u32 *elements,u32 count,u32 stride) {
    layout_calls++;last_elements=elements;last_count=count;last_stride=stride;
    return draw_success?0:-1;
}
'''
    for name in ('svga_accel_draw_mode', 'svga_accel_draw_triangles', 'svga_accel_draw_image',
                 'svga_accel_set_shaders', 'svga_accel_set_layout'):
        source += function(svga, name) + "\n"
    # Exercise the actual kernel enumeration boundary, not only UI fixtures.
    abi = (ROOT / "include/kestrel/syscall.h").read_text()
    nv_header = (ROOT / "kernel/nv.h").read_text()
    for data, name, kind in ((header, 'gpu_vendor_t', 'enum'),
                              (header, 'gpu_accel_t', 'enum'),
                              (header, 'gpu_info_t', 'struct'),
                              (abi, 'kgpuinfo_t', 'struct'),
                              (nv_header, 'nv_telemetry_t', 'struct')):
        end_type = data.index('} ' + name + ';') + len('} ' + name + ';')
        begin_type = data.rfind('typedef ' + kind + ' {', 0, end_type)
        assert begin_type >= 0
        if name == 'nv_telemetry_t':
            source += '#define NV_ENGINE_COUNT 4\n'
        source += data[begin_type:end_type] + '\n'
    source += r'''
typedef int64_t s64;
#define E_INVAL 22
#define E_NOENT 2
static gpu_info_t inventory[3];
static nv_card_t *telemetry_cards[2];
static struct { struct { u64 base,size; } fb; } g_boot;
static bool user_range_ok(u64 pointer,size_t bytes,bool write) {
    return pointer && write && bytes==sizeof(kgpuinfo_t);
}
static bool gpu_get(int index,gpu_info_t *out) {
    if(index<0 || index>=3)return false;*out=inventory[index];return true;
}
static nv_card_t *nv_card_for_pci(u8 bus,u8 slot,u8 func) {
    for(unsigned i=0;i<2;i++) {
        nv_card_t *c=telemetry_cards[i];
        if(c && c->pci_bus==bus && c->pci_slot==slot && c->pci_func==func)return c;
    }
    return NULL;
}
static void nv_telemetry_read(nv_card_t *c,nv_telemetry_t *out) {
    memset(out,0,sizeof *out);out->sampled=true;
    for(unsigned i=0;i<4;i++)out->engine_percent[i]=10*c->pci_bus+i;
    out->vram_used=100*c->pci_bus;out->temperature_c=40+c->pci_bus;
}
static const char *nv_driver_version(void) { return "fixture"; }
static const char *nv_driver_date(void) { return "fixture"; }
static size_t strlcpy(char *dst,const char *src,size_t size) {
    size_t len=strlen(src),copy=len<size?len:size-1;
    if(size){memcpy(dst,src,copy);dst[copy]=0;}return len;
}
'''
    source += function(syscall, 'enum_gpu') + '\n'
    source += r'''
typedef struct { bool up,result; unsigned fills,copies,cursors; int args[6]; u32 colour; } mock_t;
static bool mock_ready(void *ctx) { return ((mock_t *)ctx)->up; }
static bool mock_fill(void *ctx,int x,int y,int w,int h,u32 colour) {
    mock_t *m=ctx;m->fills++;m->args[0]=x;m->args[1]=y;m->args[2]=w;m->args[3]=h;m->colour=colour;return m->result;
}
static bool mock_copy(void *ctx,int sx,int sy,int dx,int dy,int w,int h) {
    mock_t *m=ctx;m->copies++;int a[]={sx,sy,dx,dy,w,h};memcpy(m->args,a,sizeof a);return m->result;
}
static void mock_cursor(void *ctx,int x,int y) {
    mock_t *m=ctx;m->cursors++;m->args[0]=x;m->args[1]=y;
}
static const gpu_accel_ops_t mock_ops={.can_fill=mock_ready,.can_copy=mock_ready,
    .can_cursor=mock_ready,.fill=mock_fill,.copy=mock_copy,.cursor_move=mock_cursor};
static const gpu_accel_ops_t missing_ops={.can_fill=mock_ready,.can_copy=mock_ready,.can_cursor=mock_ready};
static void reset_registry(void) {
    gpu_accel_clear();memset(accel_bindings,0,sizeof accel_bindings);accel_binding_count=0;
}
int main(void) {
    mock_t a={.up=true,.result=true}, b={.up=true,.result=true}, c={.up=false,.result=true};
    CHECK(!gpu_accel_capabilities() && !strcmp(gpu_accel_owner(),""));
    CHECK(!gpu_accel_register(0,32,0,&mock_ops,&a,"bad slot"));
    CHECK(!gpu_accel_register(0,1,8,&mock_ops,&a,"bad function"));
    CHECK(!gpu_accel_register(0,1,0,NULL,&a,"null ops"));
    CHECK(gpu_accel_register(0,1,0,&mock_ops,&a,"first"));
    CHECK(gpu_accel_register(0,1,1,&mock_ops,&b,"second"));
    CHECK(gpu_accel_register(1,1,0,&mock_ops,&c,"third"));
    CHECK(gpu_accel_register(1,2,0,&missing_ops,&a,NULL));
    CHECK(!gpu_accel_register(2,2,0,&mock_ops,&a,"overflow"));
    CHECK(gpu_accel_register(0,1,0,&mock_ops,&a,"duplicate"));
    CHECK(!gpu_accel_register(0,1,0,&mock_ops,&b,"conflict"));
    CHECK(!gpu_accel_register(0,1,0,&missing_ops,&a,"conflict"));
    CHECK(!gpu_accel_capabilities()); // Registration never owns the screen.
    CHECK(!gpu_accel_fill(1,2,3,4,5));
    CHECK(!gpu_accel_copy(1,2,3,4,5,6));
    CHECK(!gpu_accel_cursor_move(1,2));
    CHECK(gpu_accel_select(0,1,1) && gpu_accel_capabilities()==7);
    CHECK(!strcmp(gpu_accel_owner(),"second"));
    CHECK(gpu_accel_fill(11,12,13,14,0x12345678));
    CHECK(b.fills==1 && !a.fills && b.args[0]==11 && b.args[3]==14 && b.colour==0x12345678);
    CHECK(gpu_accel_copy(1,2,3,4,5,6));
    for(int i=0;i<6;i++)CHECK(b.args[i]==i+1);
    CHECK(b.copies==1 && !a.copies);
    CHECK(gpu_accel_cursor_move(17,18) && b.cursors==1 && !a.cursors);
    b.up=false;
    CHECK(!gpu_accel_capabilities() && !gpu_accel_fill(0,0,1,1,0));
    CHECK(!gpu_accel_copy(0,0,0,0,1,1) && !gpu_accel_cursor_move(0,0));
    CHECK(b.fills==1 && b.copies==1 && b.cursors==1 && !a.fills); // Never fall through to another GPU.
    b.up=true;b.result=false;CHECK(!gpu_accel_fill(0,0,1,1,0));
    CHECK(!gpu_accel_copy(0,0,0,0,1,1));
    CHECK(gpu_accel_select(1,2,0) && !gpu_accel_capabilities());
    CHECK(!gpu_accel_fill(0,0,1,1,0) && !gpu_accel_copy(0,0,0,0,1,1) && !gpu_accel_cursor_move(0,0));
    CHECK(!gpu_accel_select(7,7,7) && !strcmp(gpu_accel_owner(),""));
    CHECK(gpu_accel_select(0,1,0));gpu_accel_clear();CHECK(!gpu_accel_capabilities());
    reset_registry();
    nv_card_t n0={2,0,0}, n1={3,0,0};
    nv_accel_init(&n0);nv_accel_init(&n1);
    CHECK(!gpu_accel_capabilities());
    channels[CH_COPY].card=&n1;channels[CH_COPY].open=true;g_scanout_bound=true;
    CHECK(gpu_accel_select(2,0,0) && !gpu_accel_capabilities());
    CHECK(!gpu_accel_fill(0,0,1,1,0) && !gpu_accel_copy(0,0,0,0,1,1));
    CHECK(!nv_accel_use_channel(&n0,NULL) && nv_accel_use_channel(&n1,NULL));
    CHECK(gpu_accel_select(3,0,0) && gpu_accel_capabilities()==3);
    CHECK(gpu_accel_fill(0,0,1,1,0) && gpu_accel_copy(0,0,0,0,1,1));
    CHECK(nv_fill_calls==1 && nv_copy_calls==1 && !kapi_fill_calls && !kapi_copy_calls);
    runtime_ready=true;test_card=&n1;
    CHECK(nvkms_kapi_runtime_selected());
    CHECK(gpu_accel_fill(0,0,1,1,0) && gpu_accel_copy(0,0,0,0,1,1));
    CHECK(kapi_fill_calls==1 && kapi_copy_calls==1);
    test_card=&n0;
    CHECK(!nvkms_kapi_runtime_selected());
    CHECK(!gpu_accel_capabilities() && !gpu_accel_fill(0,0,1,1,0));
    CHECK(!gpu_accel_copy(0,0,0,0,1,1)); // KAPI belongs to another GPU.
    test_card=NULL;CHECK(!gpu_accel_capabilities());
    test_card=&n1;
    CHECK(!gpu_accel_fill(-1,0,1,1,0) && !gpu_accel_fill(0,0,0,1,0));
    CHECK(!gpu_accel_copy(0,0,-1,0,1,1));
    channels[CH_COPY].submit_failed=true;
    CHECK(nvkms_kapi_runtime_selected()); // Ownership persists through faults: no CPU-shadow fallback.
    CHECK(!nv_chan_display_card() && !gpu_accel_capabilities());
    CHECK(!gpu_accel_fill(0,0,1,1,0) && !gpu_accel_copy(0,0,0,0,1,1));
    CHECK(kapi_fill_calls==1 && kapi_copy_calls==1); // Stale KAPI-ready cannot override channel failure.
    channels[CH_COPY].submit_failed=false;channels[CH_COPY].card=&n0;
    CHECK(!gpu_accel_capabilities()); // Channel ownership cannot silently migrate this target.
    channels[CH_COPY].open=false;CHECK(!nv_chan_display_card());
    channels[CH_COPY].open=true;g_scanout_bound=false;CHECK(!nv_chan_display_card());
    /* Production 3D/pixel/image wrappers follow the selected context, including
     * unsupported scaling and faults: never fall through to the other driver. */
    static const gpu_accel_ops_t svga_ops={.draw_mode=svga_accel_draw_mode,
        .draw_triangles=svga_accel_draw_triangles,.draw_image=svga_accel_draw_image,
        .set_shaders=svga_accel_set_shaders,.set_layout=svga_accel_set_layout};
    CHECK(gpu_accel_register(4,0,0,&svga_ops,&svga,"VMware"));
    float vertices[72]={0};u32 pixels[64]={0},pixel=0;
    CHECK(gpu_accel_is_selected(3,0,0) && !gpu_accel_is_selected(2,0,0));
    channels[CH_COPY].card=&n1;g_scanout_bound=true;
    CHECK(!gpu_accel_draw_mode() && gpu_accel_draw_triangles(vertices,3)==-1);
    channels[CH_GFX]=(nv_channel_t){true,false,&n1};g_compute_scanout_bound=true;
    CHECK(gpu_accel_draw_mode()==2 && gpu_accel_draw_triangles(vertices,3)==3);
    CHECK(kapi_draw_calls==1 && !svga_draw_calls && last_vertices==vertices && last_triangles==3);
    CHECK(gpu_accel_draw_triangles(NULL,3)==-1 && gpu_accel_draw_triangles(vertices,0)==-1);
    channels[CH_GFX].submit_failed=true;
    CHECK(!gpu_accel_draw_mode() && gpu_accel_draw_triangles(vertices,3)==-1 && kapi_draw_calls==1);
    channels[CH_GFX].submit_failed=false;channels[CH_GFX].card=&n0;
    CHECK(!gpu_accel_draw_mode());channels[CH_GFX].card=&n1;
    CHECK(!gpu_accel_draw_image(pixels,4,3,8,7,9,0,0));
    CHECK(kapi_image_calls==1 && !svga_image_calls && last_pixels==pixels &&
        last_width==4 && last_height==3 && last_stride==8 && last_x==7 && last_y==9);
    CHECK(gpu_accel_draw_image(pixels,4,3,8,7,9,8,6)==-1 && !svga_image_calls && kapi_image_calls==1);
    CHECK(gpu_accel_draw_image(pixels,4,3,3,7,9,0,0)==-1);
    CHECK(gpu_accel_read_pixel(5,6,&pixel) && pixel==0xaabbccdd && last_x==5 && last_y==6);
    draw_success=false;
    CHECK(gpu_accel_draw_triangles(vertices,3)==-1 && !svga_draw_calls);
    CHECK(gpu_accel_draw_image(pixels,4,3,8,7,9,0,0)==-1 && !svga_image_calls);
    CHECK(!gpu_accel_read_pixel(5,6,&pixel));draw_success=true;
    CHECK(gpu_accel_select(4,0,0) && gpu_accel_draw_mode()==1);
    CHECK(!nvkms_kapi_runtime_selected()); // Other adapter selected despite NVKMS still being initialized.
    CHECK(gpu_accel_draw_triangles(vertices,3)==3 && svga_draw_calls==1 && kapi_draw_calls==2);
    CHECK(!gpu_accel_draw_image(pixels,4,3,8,7,9,8,6) && svga_image_calls==1);
    CHECK(last_draw_width==8 && last_draw_height==6 && kapi_image_calls==2);
    CHECK(gpu_accel_draw_image(pixels,1025,1,1025,0,0,0,0)==-1 && svga_image_calls==1);
    CHECK(!gpu_accel_read_pixel(0,0,&pixel) && kapi_pixel_calls==2);
    svga_up=false;CHECK(!gpu_accel_draw_mode() && gpu_accel_draw_triangles(vertices,3)==-1);
    CHECK(gpu_accel_draw_image(pixels,4,3,8,7,9,8,6)==-1 && kapi_image_calls==2);
    gpu_accel_clear();CHECK(!gpu_accel_is_selected(4,0,0) && !gpu_accel_draw_mode());
    CHECK(gpu_accel_draw_triangles(vertices,3)==-1 && gpu_accel_draw_image(pixels,4,3,8,7,9,0,0)==-1);
    CHECK(!gpu_accel_read_pixel(0,0,&pixel));
    /* Explicit format and selected adapter; NVIDIA never borrows VMware's
     * bytecode interface, and failed requests are not replayed. */
    gpu_shader_program_t vs={pixels,pixels+8,pixels+16,7,3,2};
    gpu_shader_program_t ps={pixels+24,pixels+32,pixels+40,8,2,1};
    CHECK(gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX,&vs,&ps)==-1);
    CHECK(gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX,pixels,3,32)==-1);
    CHECK(gpu_accel_select(3,0,0));svga_up=true;
    CHECK(gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX,&vs,&ps)==-1 && !shader_calls);
    CHECK(gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX,pixels,3,32)==-1 && !layout_calls);
    CHECK(gpu_accel_select(4,0,0));
    CHECK(gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX,&vs,&ps)==0 && shader_calls==1);
    CHECK(last_vs.code==vs.code && last_vs.inputs==vs.inputs && last_vs.outputs==vs.outputs &&
          last_vs.words==7 && last_vs.input_count==3 && last_vs.output_count==2);
    CHECK(last_ps.code==ps.code && last_ps.inputs==ps.inputs && last_ps.outputs==ps.outputs &&
          last_ps.words==8 && last_ps.input_count==2 && last_ps.output_count==1);
    CHECK(!gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX,pixels,3,32) && layout_calls==1 &&
          last_elements==pixels && last_count==3 && last_stride==32);
    CHECK(gpu_accel_set_shaders(99,&vs,&ps)==-1 && shader_calls==1);
    CHECK(gpu_accel_set_layout(99,pixels,3,32)==-1 && layout_calls==1);
    CHECK(svga_accel_set_shaders(&a,GPU_PROGRAM_SVGA_DX,&vs,&ps)==-1 && shader_calls==1);
    CHECK(svga_accel_set_layout(&a,GPU_PROGRAM_SVGA_DX,pixels,3,32)==-1 && layout_calls==1);
    CHECK(gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX,NULL,&ps)==-1);
    CHECK(gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX,NULL,3,32)==-1);
    CHECK(gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX,pixels,0,32)==-1);
    CHECK(gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX,pixels,3,0)==-1);
    vs.inputs=NULL;CHECK(gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX,&vs,&ps)==-1);
    vs.inputs=pixels+8;vs.words=0;CHECK(gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX,&vs,&ps)==-1);vs.words=7;
    svga_up=false;
    CHECK(gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX,&vs,&ps)==-1 && shader_calls==1);
    CHECK(gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX,pixels,3,32)==-1 && layout_calls==1);
    svga_up=true;draw_success=false;
    CHECK(gpu_accel_set_shaders(GPU_PROGRAM_SVGA_DX,&vs,&ps)==-1 && shader_calls==2);
    CHECK(gpu_accel_set_layout(GPU_PROGRAM_SVGA_DX,pixels,3,32)==-1 && layout_calls==2);
    draw_success=true;
    /* The desktop can be driven by the non-boot GPU; telemetry remains keyed
     * to each enumerated PCI identity, not the selected adapter's counters. */
    inventory[0]=(gpu_info_t){.vendor=GPU_NVIDIA,.bus=2,.is_boot_display=true,.accel=GPU_ACCEL_FRAMEBUFFER};
    inventory[1]=(gpu_info_t){.vendor=GPU_NVIDIA,.bus=3};
    inventory[2]=(gpu_info_t){.vendor=GPU_AMD,.bus=5};
    telemetry_cards[0]=&n0;telemetry_cards[1]=&n1;
    channels[CH_COPY]=(nv_channel_t){true,false,&n1};
    channels[CH_GFX]=(nv_channel_t){true,false,&n1};
    g_scanout_bound=g_compute_scanout_bound=true;runtime_ready=true;test_card=&n1;
    CHECK(gpu_accel_select(3,0,0));
    kgpuinfo_t report;
    CHECK(!enum_gpu(1,(u64)(uintptr_t)&report) && !report.boot_display && report.accel==GPU_ACCEL_2D);
    CHECK(!strcmp(report.renderer,"GPU compute raster + VRAM composition") && report.engine_percent[0]==30);
    CHECK(!enum_gpu(0,(u64)(uintptr_t)&report) && report.engine_percent[0]==20);
    CHECK(!strcmp(report.renderer,"Not selected / renderer unavailable"));
    CHECK(!enum_gpu(2,(u64)(uintptr_t)&report) && report.engine_percent[0]==-1 && !report.engines_sampled);
    channels[CH_GFX].submit_failed=true;
    CHECK(!enum_gpu(1,(u64)(uintptr_t)&report) && !strcmp(report.renderer,"GPU fills and copies"));
    channels[CH_COPY].submit_failed=true;
    CHECK(!enum_gpu(1,(u64)(uintptr_t)&report) && !strcmp(report.renderer,"Not selected / renderer unavailable"));
    gpu_accel_clear();g_boot.fb.base=1;g_boot.fb.size=4096;
    CHECK(!enum_gpu(0,(u64)(uintptr_t)&report) && !strcmp(report.renderer,"CPU framebuffer (this adapter)"));
    CHECK(enum_gpu(0,0)==-E_INVAL && enum_gpu(3,(u64)(uintptr_t)&report)==-E_NOENT);
    puts("PASS: per-device registration, explicit selection, drawing/shader/layout dispatch, format rejection, failure and NVIDIA ownership");
    return 0;
}
'''
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-accel-routing-") as directory:
        c = Path(directory) / "routing.c"
        executable = Path(directory) / "routing.exe"
        c.write_text(source)
        subprocess.run([clang, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                        str(c), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
