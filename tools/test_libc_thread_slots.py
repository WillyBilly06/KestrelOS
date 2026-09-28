#!/usr/bin/env python3
"""Stress native startup-slot publication on concurrent creators/children."""
import re
from test_gpu_stable_candidate import ROOT, run_test

src = (ROOT / 'user/libc/syscalls.c').read_text()
start = src.index('typedef struct {', src.index('/* --------------------------------------------------------------- threads */'))
end = src.index('\nvoid thread_exit(void)', start)
# Native long is 64 bits; host Windows long is only 32 bits.
body = re.sub(r'\blong\b', 'intptr_t', src[start:end])
code = r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
enum{SYS_THREAD=1,THREAD_CREATE=1};
static void thread_exit(void){ExitThread(0);}
static intptr_t syscall6(int,int,intptr_t,intptr_t,int,int,int);
'''
code += body
code += r'''
static HANDLE children[256],start_event;
static unsigned next_child,called[256];
static _Thread_local bool fail_create;
static SRWLOCK create_lock=SRWLOCK_INIT;
static DWORD WINAPI child(void*arg){thread_trampoline(arg);assert(0);return 0;}
static intptr_t syscall6(int nr,int op,intptr_t entry,intptr_t arg,int a,int b,int c){
    assert(nr==SYS_THREAD&&op==THREAD_CREATE&&entry==(intptr_t)thread_trampoline&&!a&&!b&&!c);
    assert(__atomic_load_n(&((thread_start_t*)arg)->state,__ATOMIC_ACQUIRE)==2);
    if(fail_create)return -ENOMEM;
    AcquireSRWLockExclusive(&create_lock);unsigned id=next_child++;assert(id<256);
    children[id]=CreateThread(NULL,0,child,(void*)arg,0,NULL);assert(children[id]);
    ReleaseSRWLockExclusive(&create_lock);return id+1;
}
static void callback(void*arg){
    unsigned id=(unsigned)(uintptr_t)arg;assert(id<256);
    assert(__atomic_fetch_add(&called[id],1u,__ATOMIC_RELAXED)==0);
    Sleep(0);
}
static DWORD WINAPI creator(void*raw){
    unsigned id=(unsigned)(uintptr_t)raw;assert(WaitForSingleObject(start_event,5000)==WAIT_OBJECT_0);
    for(unsigned j=0;j<32;j++){
        unsigned attempts=0;
        while(thread_create(callback,(void*)(uintptr_t)(id*32+j))<0){
            assert(errno==EAGAIN&&++attempts<100000);Sleep(0);
        }
    }
    return 0;
}
int main(void){
    assert(thread_create(NULL,NULL)==-1&&errno==EINVAL);
    fail_create=true;assert(thread_create(callback,NULL)==-1&&errno==ENOMEM);fail_create=false;
    for(unsigned i=0;i<32;i++){assert(thread_slots[i].state==0);thread_slots[i].state=1;}
    assert(thread_create(callback,NULL)==-1&&errno==EAGAIN);
    for(unsigned i=0;i<32;i++)thread_slots[i].state=0;
    HANDLE creators[8];start_event=CreateEvent(NULL,TRUE,FALSE,NULL);assert(start_event);
    for(unsigned i=0;i<8;i++){creators[i]=CreateThread(NULL,0,creator,(void*)(uintptr_t)i,0,NULL);assert(creators[i]);}
    SetEvent(start_event);assert(WaitForMultipleObjects(8,creators,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<8;i++)CloseHandle(creators[i]);CloseHandle(start_event);
    assert(next_child==256);
    for(unsigned i=0;i<256;i++){
        assert(WaitForSingleObject(children[i],5000)==WAIT_OBJECT_0);CloseHandle(children[i]);
    }
    /* OS thread handles are indexed by creation order, callback arguments by
     * creator/job. They differ under concurrency: join ALL before checking any
     * callback id rather than assuming children[i] delivered called[i]. */
    for(unsigned i=0;i<256;i++)assert(called[i]==1);
    for(unsigned i=0;i<32;i++)assert(thread_slots[i].state==0);
    puts("PASS native thread startup: 8 concurrent creators, 256 exact callback/argument deliveries, slot exhaustion, failure release and reuse; host child launcher substitutes for native syscall");
}
'''
if __name__ == '__main__':
    run_test(code, 'libc-thread-slots')
