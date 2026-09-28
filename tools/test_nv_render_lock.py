#!/usr/bin/env python3
"""Exercise production transaction guard and all legacy native entry wrappers."""
from pathlib import Path
from test_gpu_stable_candidate import function, run_test

ROOT=Path(__file__).resolve().parents[1]
CHAN=(ROOT/'kernel/nv_chan.c').read_text()
KAPI=(ROOT/'kernel/nvkms_kapi_client.c').read_text()

if __name__=='__main__':
    c=r'''
#include <windows.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;typedef int32_t s32;
static u32 g_render_transaction;
typedef struct {unsigned pins;} proc_t;
static proc_t *g_render_pin;
static __declspec(thread) proc_t thread_pin;
static bool proc_resource_pin(proc_t **token) {thread_pin.pins++;*token=&thread_pin;return true;}
static void proc_resource_unpin(proc_t *token) {assert(token==&thread_pin && token->pins);token->pins--;}
'''
    c+=function(CHAN,'nv_render_try_begin')+'\n'+function(CHAN,'nv_render_end')
    c+=r'''
static bool result;
static unsigned calls;
static void entered(void) {assert(g_render_transaction==1);assert(!nv_render_try_begin());calls++;}
static bool runtime_read_pixel_locked(s32 x,s32 y,u32 *p) {(void)x;(void)y;(void)p;entered();return result;}
static bool runtime_present_locked(const u32 *p,u32 w,u32 h,u32 s,s32 x,s32 y) {(void)p;(void)w;(void)h;(void)s;(void)x;(void)y;entered();return result;}
static bool runtime_fill_locked(s32 x,s32 y,s32 w,s32 h,u32 c) {(void)x;(void)y;(void)w;(void)h;(void)c;entered();return result;}
static bool runtime_copy_locked(s32 sx,s32 sy,s32 dx,s32 dy,s32 w,s32 h) {(void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;entered();return result;}
static int runtime_draw_locked(const float *p,u32 n) {(void)p;entered();return result?(int)n:-1;}
'''
    for name in ['read_pixel','present','fill','copy','draw']:
        c+='\n'+function(KAPI,'nvkms_kapi_runtime_'+name)
    c+=r'''
static u32 critical,successes;
static DWORD WINAPI contender(void *unused) {
    (void)unused;
    for(unsigned i=0;i<100000;i++) {
        if(!nv_render_try_begin()) continue;
        assert(__atomic_add_fetch(&critical,1,__ATOMIC_RELAXED)==1);
        __atomic_add_fetch(&successes,1,__ATOMIC_RELAXED);
        assert(__atomic_sub_fetch(&critical,1,__ATOMIC_RELAXED)==0);
        nv_render_end();
    }
    return 0;
}
int main(void) {
    for(int r=0;r<2;r++) {
        result=r;calls=0;
        assert(nvkms_kapi_runtime_read_pixel(0,0,NULL)==result && !g_render_transaction);
        assert(nvkms_kapi_runtime_present(NULL,1,1,1,0,0)==result && !g_render_transaction);
        assert(nvkms_kapi_runtime_fill(0,0,1,1,0)==result && !g_render_transaction);
        assert(nvkms_kapi_runtime_copy(0,0,1,1,1,1)==result && !g_render_transaction);
        assert(nvkms_kapi_runtime_draw(NULL,3)==(r?3:-1) && !g_render_transaction);
        assert(calls==5);
    }
    assert(nv_render_try_begin());calls=0;
    assert(!nvkms_kapi_runtime_read_pixel(0,0,NULL));
    assert(!nvkms_kapi_runtime_present(NULL,1,1,1,0,0));
    assert(!nvkms_kapi_runtime_fill(0,0,1,1,0));
    assert(!nvkms_kapi_runtime_copy(0,0,1,1,1,1));
    assert(nvkms_kapi_runtime_draw(NULL,1)==-1);
    assert(!calls && g_render_transaction==1);nv_render_end();
    HANDLE threads[8];
    for(int i=0;i<8;i++) {threads[i]=CreateThread(NULL,0,contender,NULL,0,NULL);assert(threads[i]);}
    assert(WaitForMultipleObjects(8,threads,TRUE,30000)==WAIT_OBJECT_0);
    for(int i=0;i<8;i++)CloseHandle(threads[i]);
    assert(successes && !critical && !g_render_transaction);
    printf("PASS production render transaction guard: eight threads/800000 attempts; %u admissions; all five frontend wrappers preserve/release ownership on busy/success/failure\n",successes);
}
'''
    run_test(c,'nv_render_lock')
