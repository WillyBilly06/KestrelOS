#!/usr/bin/env python3
"""Actual desktop API demos; strict mocked Vulkan/COM ownership and failures.

This checks application lifetimes, not implementation/conformance of the APIs
or native GPU rendering. Every constructor and fallible Vulkan setup operation
can fail; successful retries must not leak or retain an obsolete target.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from test_gl_gpu import strip_includes

ROOT=Path(__file__).resolve().parents[1]
HEADER=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef struct {int width,height;} surface_t;
static float radiansf(float x){return x*0.017453292519943295f;}
static size_t strlcpy(char *d,const char *s,size_t n){size_t z=strlen(s);if(n){size_t m=z<n-1?z:n-1;memcpy(d,s,m);d[m]=0;}return z;}
'''
OBJECTS=r'''
typedef struct object {
    const void *vtable;
    unsigned kind;
    struct object *owner,*dependency,*bound;
    surface_t *target;
    unsigned char *data;
    size_t bytes;
} object;
static object *objects[64];
static unsigned live,created,released,edge_count,fail_edge,draws,bindings;
static surface_t *expected_target;
static bool partial_device_failure;
static bool call_failed;
static unsigned runtime_failure,end_scenes;
static unsigned expected_vertices=36;
static object *check(const void *p){assert(p);for(unsigned i=0;i<64;i++)if(objects[i]==p)return objects[i];assert(!"dead/foreign API handle");return NULL;}
static object *make(unsigned kind,void *owner){
    unsigned i=0;while(i<64&&objects[i])i++;assert(i<64);
    object *p=calloc(1,sizeof(*p));assert(p);p->kind=kind;p->owner=owner?check(owner):NULL;
    p->bytes=2048;p->data=calloc(1,p->bytes);assert(p->data);
    objects[i]=p;live++;created++;return p;
}
static void drop(void *ptr){
    if(!ptr)return;
    object *p=check(ptr);
    for(unsigned i=0;i<64;i++)if(objects[i]&&objects[i]!=p){
        assert(objects[i]->owner!=p&&objects[i]->dependency!=p&&objects[i]->bound!=p);
    }
    for(unsigned i=0;i<64;i++)if(objects[i]==p){objects[i]=NULL;break;}
    free(p->data);free(p);live--;released++;
}
static bool edge(void){if(++edge_count!=fail_edge)return false;call_failed=true;return true;}
static void rendered(surface_t *target){assert(target==expected_target);draws++;}
'''

def vk_mocks(header,source):
    used=set(re.findall(r'\b(vk[A-Z]\w*)\s*\(',source))
    bodies={
        'vkCreateInstance':'if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=(void*)make(1,NULL);return VK_SUCCESS;',
        'vkDestroyInstance':'drop(instance);',
        'vkEnumeratePhysicalDevices':'check(instance);if(edge())return VK_ERROR_INITIALIZATION_FAILED;*count=1;*out=(void*)instance;return VK_SUCCESS;',
        'vkGetPhysicalDeviceProperties':'check(dev);memset(out,0,sizeof(*out));strlcpy(out->deviceName,"mock GPU",sizeof out->deviceName);',
        'vkCreateDevice':'if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=(void*)make(2,phys);return VK_SUCCESS;',
        'vkDestroyDevice':'drop(device);',
        'vkGetDeviceQueue':'check(device);*out=(void*)device;',
        'vkGetBufferMemoryRequirements':'check(d);check(b);memset(out,0,sizeof(*out));out->size=check(b)->bytes;',
        'vkBindBufferMemory':'check(d);check(b);check(m);if(edge())return VK_ERROR_INITIALIZATION_FAILED;check(b)->dependency=check(m);return VK_SUCCESS;',
        'vkMapMemory':'check(d);check(m);if(edge())return VK_ERROR_MEMORY_MAP_FAILED;assert(offset+size<=check(m)->bytes);*out=check(m)->data+offset;return VK_SUCCESS;',
        'vkUnmapMemory':'check(d);check(m);',
        'vkBindImageToSurface':'check(image);if(edge())return VK_ERROR_INITIALIZATION_FAILED;check(image)->target=colour_surface;bindings++;return VK_SUCCESS;',
        'vkCreateImageView':'if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;object *p=make(6,d);p->dependency=check(info->image);p->target=check(info->image)->target;*out=(void*)p;return VK_SUCCESS;',
        'vkCreateFramebuffer':'if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;object *p=make(8,d);p->dependency=check(info->pAttachments[0]);p->target=p->dependency->target;check(info->renderPass);*out=(void*)p;return VK_SUCCESS;',
        'vkCreateGraphicsPipelines':'assert(count==1);check(infos->layout);check(infos->renderPass);if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=(void*)make(10,d);return VK_SUCCESS;',
        'vkAllocateCommandBuffers':'check(d);check(info->commandPool);if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=(void*)make(12,info->commandPool);return VK_SUCCESS;',
        'vkFreeCommandBuffers':'check(d);check(p);for(unsigned i=0;i<count;i++)drop(bufs[i]);',
        'vkBeginCommandBuffer':'check(cb);return runtime_failure==1?VK_ERROR_INITIALIZATION_FAILED:VK_SUCCESS;',
        'vkEndCommandBuffer':'check(cb);return runtime_failure==2?VK_ERROR_INITIALIZATION_FAILED:VK_SUCCESS;',
        'vkCmdBeginRenderPass':'check(cb)->target=check(info->framebuffer)->target;check(info->renderPass);',
        'vkCmdEndRenderPass':'check(cb);',
        'vkCmdBindPipeline':'check(cb);check(p);',
        'vkCmdSetViewport':'check(cb);',
        'vkCmdSetTransformKESTREL':'check(cb);',
        'vkCmdBindVertexBuffers':'check(cb);for(unsigned i=0;i<count;i++)check(buffers[i]);',
        'vkCmdDraw':'check(cb);assert(vertexCount==expected_vertices);',
        'vkQueueSubmit':'check(q);assert(count==1);if(runtime_failure==3)return VK_ERROR_INITIALIZATION_FAILED;rendered(check(submits->pCommandBuffers[0])->target);return VK_SUCCESS;',
        'vkQueueWaitIdle':'check(q);return runtime_failure==4?VK_ERROR_INITIALIZATION_FAILED:VK_SUCCESS;',
        'vkDeviceWaitIdle':'check(d);return VK_SUCCESS;',
    }
    for name,kind in [('Buffer',3),('Image',5),('RenderPass',7),('PipelineLayout',9),('CommandPool',11)]:
        bodies['vkCreate'+name]=f'check(d);if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;*out=(void*)make({kind},d);return VK_SUCCESS;'
    bodies['vkCreateBuffer']='check(d);if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;object *p=make(3,d);p->bytes=info->size;*out=(void*)p;return VK_SUCCESS;'
    bodies['vkAllocateMemory']='check(d);if(edge())return VK_ERROR_OUT_OF_HOST_MEMORY;object *p=make(4,d);free(p->data);p->bytes=info->allocationSize;p->data=calloc(1,p->bytes);assert(p->data);*out=(void*)p;return VK_SUCCESS;'
    for name,param in [('Buffer','b'),('Image','i'),('ImageView','v'),('RenderPass','r'),('Framebuffer','f'),('PipelineLayout','l'),('Pipeline','p'),('CommandPool','p')]:
        bodies['vkDestroy'+name]=f'check(d);drop({param});'
    bodies['vkFreeMemory']='check(d);drop(m);'
    out=[]
    for ret,name,args in re.findall(r'^(VkResult|void)\s+(vk\w+)\((.*?)\);',header,re.M|re.S):
        if name not in used:continue
        assert name in bodies,name
        out.append(f'{ret} {name}({args}){{{bodies[name]}}}')
        used.remove(name)
    assert not used,used
    return '\n'.join(out)

def com_mocks(header):
    names=['IDirect3D9','IDirect3DDevice9','ID3D11Device','ID3D11DeviceContext']
    source='\n'.join(f'static const {name}Vtbl mock_{name};' for name in names)+'\n'
    clean=re.sub(r'/\*.*?\*/','',header,flags=re.S)
    for name in names:
        block=re.search(r'typedef struct \{([^{}]+)\} '+name+r'Vtbl;',clean,re.S).group(1)
        entries=[]
        for ret,method,args in re.findall(r'(HRESULT|UINT|void)\s*\(\*(\w+)\)\((.*?)\);',block,re.S):
            body='check(self);'+('return S_OK;' if ret=='HRESULT' else 'return 1;' if ret=='UINT' else '')
            if method=='Release':body='drop(self);return S_OK;'
            if method=='CreateDevice':body='check(self);*out=NULL;if(edge())return E_OUTOFMEMORY;object *p=make(21,self);p->vtable=&mock_IDirect3DDevice9;*out=(void*)p;return S_OK;'
            if method=='DrawPrimitiveUP':body='assert(primitive_count==12);check(self);if(runtime_failure==8)return E_FAIL;rendered(check(self)->target);return S_OK;'
            if method=='Clear':body='check(self);return runtime_failure==6?E_FAIL:S_OK;'
            if method=='BeginScene':body='check(self);return runtime_failure==7?E_FAIL:S_OK;'
            if method=='EndScene':body='check(self);end_scenes++;return runtime_failure==9?E_FAIL:S_OK;'
            if method=='Present':body='check(self);return runtime_failure==10?E_FAIL:S_OK;'
            if method=='CreateBuffer':body='check(self);*out=NULL;if(edge())return E_OUTOFMEMORY;*out=(void*)make(24,self);return S_OK;'
            if method=='CreateInputLayout':body='check(self);*out=NULL;if(edge())return E_OUTOFMEMORY;*out=(void*)make(25,self);return S_OK;'
            if method=='CreateRenderTargetViewFromSurfaceKESTREL':body='check(self);*out=NULL;if(edge())return E_OUTOFMEMORY;object *p=make(26,self);p->target=colour_surface;*out=(void*)p;return S_OK;'
            if method=='OMSetRenderTargets':body='check(self)->bound=count?check(rtvs[0]):NULL;'
            if method=='ClearRenderTargetView':body='assert(check(self)->bound==check(rtv));'
            if method=='IASetInputLayout':body='check(self);check(layout);'
            if method=='IASetVertexBuffers':body='check(self);for(unsigned i=0;i<count;i++)check(buffers[i]);'
            if method=='Draw':body='assert(vertex_count==36);rendered(check(check(self)->bound)->target);'
            fn=f'mock_{name}_{method}'
            source+=f'static {ret} {fn}({args}){{{body}}}\n'
            entries.append(f'.{method}={fn}')
        source+=f'static const {name}Vtbl mock_{name}={{'+','.join(entries)+'};\n'
    return source+r'''
IDirect3D9 *Direct3DCreate9(UINT version){assert(version==D3D_SDK_VERSION);if(edge())return NULL;object *p=make(20,NULL);p->vtable=&mock_IDirect3D9;return (void*)p;}
HRESULT D3D9SetTargetKESTREL(IDirect3DDevice9 *device,void *target){check(device);if(runtime_failure==5)return E_FAIL;check(device)->target=target;bindings++;return S_OK;}
HRESULT D3D11CreateDevice(void *a,UINT type,void *s,UINT flags,const void *levels,UINT n,UINT sdk,ID3D11Device **device,UINT *level,ID3D11DeviceContext **context){
    *device=NULL;*context=NULL;
    if(edge())return E_OUTOFMEMORY;
    object *d=make(22,NULL);d->vtable=&mock_ID3D11Device;*device=(void*)d;
    if(partial_device_failure)return E_OUTOFMEMORY;
    object *c=make(23,d);c->vtable=&mock_ID3D11DeviceContext;*context=(void*)c;*level=0xb000;return S_OK;
}
void D3D11ReleaseBufferKESTREL(ID3D11Buffer *b){drop(b);}
void D3D11ReleaseInputLayoutKESTREL(ID3D11InputLayout *p){drop(p);}
void D3D11ReleaseRenderTargetViewKESTREL(ID3D11RenderTargetView *v){drop(v);}
'''

TEST=r'''
static bool zero(const void *p,size_t n){for(size_t i=0;i<n;i++)if(((const unsigned char*)p)[i])return false;return true;}
typedef bool (*draw_fn)(void*,surface_t*,int,int,int,float,float);
typedef void (*destroy_fn)(void*);
static draw_fn draw_api[]={api_draw_vulkan,api_draw_d3d9,api_draw_d3d11};
static destroy_fn destroy_api[]={api_destroy_vulkan,api_destroy_d3d9,api_destroy_d3d11};
static size_t state_bytes[]={sizeof(vk_demo_t),sizeof(d3d9_demo_t),sizeof(d3d11_demo_t)};
static unsigned failure_cases;
static void render(unsigned api,void *state,surface_t *target){
    expected_target=target;call_failed=false;
    bool ok=draw_api[api](state,target,7,target->width,target->height-7,0.3f,0.1f);
    assert(ok==!(call_failed||partial_device_failure||runtime_failure));
}
int main(void){
    surface_t a={320,240},b={320,240},c={640,360};
    size_t bytes=api_state_size();void *state=calloc(1,bytes);assert(state);
    for(unsigned api=0;api<3;api++){
        unsigned first=draws;edge_count=fail_edge=0;render(api,state,&a);
        assert(draws==first+1&&live);unsigned setup_edges=edge_count;
        unsigned same=created;render(api,state,&a);assert(created==same);
        render(api,state,&b);render(api,state,&c);
        destroy_api[api](state);destroy_api[api](state);destroy_api[api](NULL);
        assert(!live&&zero(state,state_bytes[api]));
        for(unsigned failure=1;failure<=setup_edges;failure++){
            edge_count=0;fail_edge=failure;first=draws;render(api,state,&a);
            assert(edge_count==failure&&draws==first);
            // RTV creation is retryable with a live, unbound D3D11 context.
            if(api==2&&failure==setup_edges){d3d11_demo_t *d=state;assert(d->ready&&!d->rtv&&!d->target&&!check(d->context)->bound);}
            else assert(!live&&zero(state,state_bytes[api]));
            fail_edge=0;render(api,state,&a);assert(draws==first+1);
            destroy_api[api](state);assert(!live&&zero(state,state_bytes[api]));failure_cases++;
        }
        // Repeated close and same-size target changes must not accumulate resources.
        for(unsigned repeat=0;repeat<100;repeat++){
            render(api,state,&a);render(api,state,&b);destroy_api[api](state);assert(!live);
        }
    }
    partial_device_failure=true;render(2,state,&a);assert(!live&&zero(state,sizeof(d3d11_demo_t)));
    partial_device_failure=false;render(2,state,&a);api_destroy_d3d11(state);assert(!live);
    // Failed RTV replacement must unbind/release the old view and clear target;
    // going back to the old same-size surface must allocate a fresh view.
    render(2,state,&a);d3d11_demo_t *d=state;
    edge_count=0;fail_edge=1;unsigned first=draws;render(2,state,&b);
    assert(draws==first&&d->ready&&!d->target&&!d->rtv&&!check(d->context)->bound);
    fail_edge=0;render(2,state,&a);assert(draws==first+1&&d->target==&a);
    api_destroy_d3d11(state);assert(!live);
    for(unsigned stage=1;stage<=10;stage++){
        unsigned api=stage<=4?0:1;render(api,state,&a);
        runtime_failure=stage;unsigned ended=end_scenes;
        render(api,state,stage==5?&b:&a);
        if(stage==8)assert(end_scenes==ended+1); // failed draw still exits begun scene
        runtime_failure=0;destroy_api[api](state);assert(!live);
    }
    free(state);
    // Actual shape reconfiguration, all capacities and target-resize handling.
    vk_demo_t varied={0};
    for(unsigned shape=0;shape<6;shape++)for(unsigned detail=0;detail<4;detail++){
        assert(api_config_vulkan(&varied,shape,detail));
        expected_vertices=vk_mesh_count(shape,detail);
        expected_target=&a;assert(api_draw_vulkan(&varied,&a,0,640,480,0,0));
        assert(varied.vertex_count==expected_vertices&&varied.shape==shape&&varied.detail==detail);
        unsigned allocations=created;
        assert(api_config_vulkan(&varied,shape,detail));
        assert(api_draw_vulkan(&varied,&a,0,640,480,1,0));
        assert(created==allocations); // no geometry allocation/upload on animation
        expected_target=&b;assert(api_draw_vulkan(&varied,&b,0,640,480,1,0));
        assert(varied.vertex_count==expected_vertices&&varied.shape==shape&&varied.detail==detail);
        assert(!api_config_vulkan(&varied,6,0)&&!api_config_vulkan(&varied,0,4));
    }
    api_destroy_vulkan(&varied);assert(!live);expected_vertices=36;
    // Compile and invoke the actual app-level API release helper, not a copy.
    for(unsigned api=0;api<3;api++){
        glapp_t app={.api_state=calloc(1,bytes),.api_state_for=(int)api+1};assert(app.api_state);
        render(api,app.api_state,&a);api_release(&app);api_release(&app);
        assert(!app.api_state&&app.api_state_for==API_GL&&!live);
    }
    assert(created==released);
    printf("PASS desktop API lifetimes: %u setup failure/retry edges, partial D3D11 device, RTV failure/rebind, 300 reopen cycles, same-size target changes, app release helper, 10 Vulkan/D3D9 runtime failures; %u objects released\n",failure_cases, released);
}
'''

if __name__=='__main__':
    vk=(ROOT/'user/libgl/vulkan.h').read_text()
    d3d=(ROOT/'user/libgl/d3d.h').read_text()
    source=strip_includes((ROOT/'user/desktop/app_gl_apis.c').read_text())
    source=source.replace('} api_vertex_t;', '} api_vertex_t;\n'+(ROOT/'user/desktop/vulkan_mesh.h').read_text())
    app=(ROOT/'user/desktop/app_gl.c').read_text()
    release=app[app.index('static void api_release('):app.index('static void frame_finished(')]
    # App event/control-flow integration is additionally guarded at source level;
    # the helper itself and every demo lifecycle run as compiled production C.
    assert 'api_release(a);' in app[app.index('case WE_CLOSE:'):]
    assert 'a->api_state_for != a->api' in app
    integration='enum {API_GL,API_VULKAN,API_D3D9,API_D3D11};typedef struct {void *api_state;int api_state_for;} glapp_t;\n'+release
    clang=shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    with tempfile.TemporaryDirectory(prefix='kestrel-api-lifetime-') as tmp:
        path=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe'
        path.write_text(HEADER+strip_includes(vk)+strip_includes(d3d)+OBJECTS+vk_mocks(vk,source)+com_mocks(d3d)+source+integration+TEST)
        subprocess.run([clang,'-std=c11','-O1','-Wall','-Wextra','-Wno-unused-parameter',str(path),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        if '--negative-controls' in sys.argv:
            controls=[
                ('partial Vulkan cleanup',source.replace('static void vk_teardown(vk_demo_t *v) {','static void vk_teardown(vk_demo_t *v) { if(!v->ready)return;',1),integration),
                ('same-size Vulkan target',source.replace('v->target != target || ', '',1),integration),
                ('D3D9 failed device cleanup',source.replace('api_destroy_d3d9(d);','',1),integration),
                ('partial D3D11 device cleanup',source.replace('api_destroy_d3d11(d);','',1),integration),
                ('D3D11 bound RTV release',source.replace('d->context->lpVtbl->OMSetRenderTargets(d->context, 0, NULL, NULL);','',1),integration),
                ('app API release',source,re.sub(r'api_destroy_(vulkan|d3d9|d3d11)\(a->api_state\);','',integration)),
            ]
            for label,broken,helper in controls:
                assert broken!=source or helper!=integration,label
                path.write_text(HEADER+strip_includes(vk)+strip_includes(d3d)+OBJECTS+vk_mocks(vk,broken)+com_mocks(d3d)+broken+helper+TEST)
                subprocess.run([clang,'-std=c11','-O1','-Wno-unused-parameter',str(path),'-o',str(exe)],check=True)
                result=subprocess.run([str(exe)],capture_output=True,text=True)
                assert result.returncode and 'Assertion failed' in result.stderr,(label,result.returncode,result.stdout,result.stderr)
                print('PASS negative control rejected:',label)
