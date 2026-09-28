#!/usr/bin/env python3
"""Stress the actual native allocator on host threads; mock sbrk/futex only."""
import re
from test_gpu_stable_candidate import ROOT, run_test

src = (ROOT / 'user/libc/malloc.c').read_text().replace('#include "kestrel.h"', '')
for name in ('malloc', 'calloc', 'realloc', 'free'):
    src = re.sub(r'\b' + name + r'\b', 'k_' + name, src)
code = r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
void *k_malloc(size_t);void *k_calloc(size_t,size_t);void *k_realloc(void*,size_t);void k_free(void*);
static _Alignas(16) unsigned char arena[64*1024*1024];
static size_t brk_at;
static bool oom;
static void *sbrk(intptr_t delta){
    if(oom&&delta>0)return(void*)-1;
    if(delta>=0){if((size_t)delta>sizeof arena-brk_at)return(void*)-1;}
    else if((uintptr_t)0-(uintptr_t)delta>brk_at)return(void*)-1;
    void*old=arena+brk_at;brk_at+=delta;return old;
}
static SRWLOCK futex_lock=SRWLOCK_INIT;
static CONDITION_VARIABLE futex_changed=CONDITION_VARIABLE_INIT;
static int futex_wait(volatile uint32_t*word,uint32_t expect,int timeout){
    assert(timeout==-1);AcquireSRWLockExclusive(&futex_lock);
    if(__atomic_load_n(word,__ATOMIC_ACQUIRE)==expect)
        assert(SleepConditionVariableSRW(&futex_changed,&futex_lock,5000,0));
    ReleaseSRWLockExclusive(&futex_lock);return 0;
}
static int futex_wake(volatile uint32_t*word,int count){
    (void)word;assert(count==1);AcquireSRWLockExclusive(&futex_lock);
    WakeConditionVariable(&futex_changed);ReleaseSRWLockExclusive(&futex_lock);return 1;
}
'''
code += src
code += r'''
static HANDLE start;
static DWORD WINAPI worker(void*raw){
    unsigned id=(unsigned)(uintptr_t)raw;
    assert(WaitForSingleObject(start,5000)==WAIT_OBJECT_0);
    unsigned char*slots[8]={0};size_t sizes[8]={0};
    for(unsigned i=0;i<2000;i++){
        unsigned s=i%8;size_t n=(i*101u+id*293u)%32768+1;
        unsigned char value=(unsigned char)(id*8+s+1);
        if(slots[s]){
            for(size_t j=0;j<sizes[s];j++)assert(slots[s][j]==value);
            unsigned char*p=k_realloc(slots[s],n);assert(p);
            for(size_t j=0;j<(n<sizes[s]?n:sizes[s]);j++)assert(p[j]==value);
            slots[s]=p;
        }else{
            slots[s]=k_calloc(n,1);assert(slots[s]);
            for(size_t j=0;j<n;j++)assert(!slots[s][j]);
        }
        assert(!((uintptr_t)slots[s]&15));sizes[s]=n;memset(slots[s],value,n);
        if(!(i%3)){k_free(slots[s]);slots[s]=NULL;sizes[s]=0;}
        if(!(i%17))Sleep(0);
    }
    for(unsigned s=0;s<8;s++)k_free(slots[s]);return 0;
}
int main(void){
    assert(!k_malloc(SIZE_MAX)&&!heap_mutex&&!head&&!brk_at);
    assert(!k_calloc(SIZE_MAX,2));
    unsigned char*p=k_malloc(32);assert(p);memset(p,0x55,32);
    assert(!k_realloc(p,SIZE_MAX));for(int i=0;i<32;i++)assert(p[i]==0x55);
    oom=true;assert(!k_malloc(2*1024*1024)&&!heap_mutex);oom=false;k_free(p);
    HANDLE threads[8];start=CreateEvent(NULL,TRUE,FALSE,NULL);assert(start);
    for(unsigned i=0;i<8;i++){threads[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)i,0,NULL);assert(threads[i]);}
    SetEvent(start);assert(WaitForMultipleObjects(8,threads,TRUE,30000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<8;i++)CloseHandle(threads[i]);CloseHandle(start);
    assert(!heap_mutex&&head&&head==tail&&head->magic==MAGIC_FREE&&!head->next&&!head->prev);
    assert((char*)(head+1)+head->size==heap_end&&heap_end==(char*)arena+brk_at);
    assert(brk_at<3*65536); // coalesced free tail returned, not permanently retained
    puts("PASS native allocator: 8 host threads / 16000 iterations, payload isolation, realloc preservation, zeroing/alignment, overflow/OOM unlocks, coalescing and tail release; host sbrk/futex substitutes");
}
'''
if __name__ == '__main__':
    run_test(code, 'libc-heap-threads')
