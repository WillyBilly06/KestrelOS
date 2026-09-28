#!/usr/bin/env python3
"""Production memory syscall arithmetic/transaction checks; mocked page storage."""
from test_gpu_stable_candidate import ROOT, run_test

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64; typedef int64_t s64;
#define PAGE_SIZE 4096ULL
#define PAGE_MASK 4095ULL
#define PAGE_ALIGN_UP(x) (((x)+PAGE_MASK)&~PAGE_MASK)
#define PAGE_ALIGN_DOWN(x) ((x)&~PAGE_MASK)
#define USER_IMAGE_BASE 0x400000ULL
#define USER_HEAP_MAX 0x50000000ULL
#define USER_MMAP_BASE 0x60000000ULL
#define USER_TSTACK_BASE 0x7ff000000000ULL
#define USER_STACK_TOP 0x7ffffff00000ULL
enum{SYS_SBRK=17,SYS_MMAP,SYS_MUNMAP};
enum{E_INVAL=22,E_BUSY=16,E_NOMEM=12};
enum{PTE_U=4,PTE_W=2,PTE_NX=128};
static struct{bool has_nx;}g_cpu={true};
typedef struct proc{u64 pml4,heap_base,heap_end,mmap_next;struct proc*leader;}proc_t;
static proc_t procs[2],*current;
static bool held,refuse,fail_alloc,fail_map;
static unsigned begins,ends,maps,frees,unmaps;
static u64 unmap_base,unmap_len;
static proc_t *proc_current(void){return current;}
static proc_t *proc_shared(proc_t*p){return p->leader?p->leader:p;}
static bool proc_vm_begin(proc_t**p){assert(!held);begins++;*p=NULL;if(refuse)return false;held=true;*p=current;return true;}
static void proc_vm_end(proc_t*p){assert(held&&p==current);held=false;ends++;}
static u64 vmm_translate(u64 p,u64 a){(void)p;(void)a;assert(held);return 0;}
static u64 pmm_alloc_zeroed(void){assert(held);return fail_alloc?0:0x1000;}
static void pmm_free_page(u64 p){assert(held&&p==0x1000);frees++;}
static bool vmm_map(u64 p,u64 v,u64 a,u64 f){(void)p;(void)v;(void)a;(void)f;assert(held);maps++;return !fail_map;}
static void vmm_unmap_user_range(u64 p,u64 v,size_t n){(void)p;assert(held);unmaps++;unmap_base=v;unmap_len=n;}
'''
code += (ROOT / 'kernel/process_memory.h').read_text()
code += r'''
static void reset(void){
    memset(procs,0,sizeof procs);current=&procs[0];procs[1].leader=current;
    procs[0].pml4=procs[1].pml4=0x1000;procs[0].heap_base=procs[0].heap_end=0x10000000;
    procs[0].mmap_next=USER_MMAP_BASE;held=refuse=fail_alloc=fail_map=false;
    begins=ends=maps=frees=unmaps=0;
}
static s64 op(u64 nr,u64 a,u64 b){return process_memory_op(current,nr,a,b,0);}
int main(void){
    reset();u64 base=current->heap_end;
    assert((u64)op(SYS_SBRK,4096,0)==base&&procs[0].heap_end==base+4096);
    current=&procs[1];assert((u64)op(SYS_SBRK,4096,0)==base+4096&&procs[0].heap_end==base+8192);
    assert((u64)op(SYS_SBRK,1ULL<<63,0)==base+8192&&procs[0].heap_end==base);
    assert(unmap_base==base&&unmap_len==8192&&!held&&begins==ends);
    reset();base=current->heap_end;
    assert((u64)syscall_brk(current,base+8192)==base+8192&&begins==1&&ends==1);
    assert((u64)syscall_brk(current,0)==base+8192&&begins==2&&ends==2);
    assert((u64)syscall_brk(current,base-1)==base+8192);
    assert((u64)syscall_brk(current,UINT64_MAX)==base+8192);
    fail_alloc=true;assert((u64)syscall_brk(current,base+16384)==base+8192);
    reset();base=USER_MMAP_BASE;
    assert((u64)op(SYS_MMAP,0,1)==base);current=&procs[1];
    assert((u64)op(SYS_MMAP,0,4096)==base+8192&&procs[0].mmap_next==base+16384);
    unsigned before=maps;
    assert(op(SYS_MMAP,UINT64_MAX-4095,8192)==-E_INVAL);
    assert(op(SYS_MMAP,0,UINT64_MAX)==-E_INVAL&&maps==before);
    procs[0].mmap_next=USER_TSTACK_BASE-4096;
    assert(op(SYS_MMAP,0,4096)==-E_NOMEM&&maps==before);
    assert(op(SYS_MUNMAP,UINT64_MAX-4095,8192)==-E_INVAL&&!unmaps);
    assert(op(SYS_MUNMAP,USER_MMAP_BASE+17,1)==0&&unmap_base==USER_MMAP_BASE&&unmap_len==4096);
    reset();fail_map=true;base=current->heap_end;
    assert(op(SYS_SBRK,4096,0)==-E_NOMEM&&frees==1&&current->heap_end==base);
    assert(!held&&begins==ends);
    reset();refuse=true;assert(op(SYS_MMAP,0,4096)==-E_BUSY&&!maps&&!ends);
    puts("PASS production VM syscalls: shared sibling cursors, absolute brk single transaction, INT64_MIN shrink, mmap overflow/guard, all failure unlocks; page allocator mocked");
}
'''
if __name__ == '__main__':
    native = (ROOT / 'kernel/syscall.c').read_text()
    linux = (ROOT / 'kernel/linux_abi.c').read_text()
    assert 'return process_memory_op(p, nr, a0, a1, a2);' in native
    assert 'return syscall_brk(p, a0);' in linux
    run_test(code, 'process-memory')
