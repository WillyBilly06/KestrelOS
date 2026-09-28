#!/usr/bin/env python3
"""Run actual page-table and framebuffer handoff code with fault injection.

The host replaces physical RAM/TLB operations, not the page-table algorithms.
This does not emulate cross-core TLB shootdown or prove live NVIDIA modesets.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    vmm = (ROOT / 'kernel/vmm.c').read_text()
    syscall = (ROOT / 'kernel/syscall.c').read_text().replace('static s64 ', 'static long ')
    mm = (ROOT / 'kernel/mm.h').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int64_t s64;
#define PAGE_SIZE 4096ULL
#define PAGE_MASK 4095ULL
#define PAGE_ALIGN_DOWN(x) ((x)&~PAGE_MASK)
#define PAGE_ALIGN_UP(x) (((x)+PAGE_MASK)&~PAGE_MASK)
static unsigned char ram[1024*4096];
static bool allocated[1024];
static unsigned freed[1024], alloc_at, tlbs, fail_alloc;
static u64 kernel_pml4;
static void *phys_to_virt(u64 p){assert(p<sizeof ram);return ram+p;}
static u64 pmm_alloc_zeroed(void){
    if(fail_alloc && ++alloc_at==fail_alloc)return 0;
    for(unsigned i=1;i<256;i++)if(!allocated[i]){
        allocated[i]=true;memset(ram+i*4096,0,4096);return i*4096ULL;
    }
    return 0;
}
static void pmm_free_page(u64 p){
    assert(!(p&PAGE_MASK)&&p<sizeof ram&&allocated[p/4096]);
    allocated[p/4096]=false;freed[p/4096]++;
}
static void invlpg(u64 va){(void)va;tlbs++;}
static bool vm_locked;
static bool vmm_lock(void){assert(!vm_locked);vm_locked=true;return false;}
static void vmm_unlock(bool irq){assert(vm_locked&&!irq);vm_locked=false;}
static void smp_tlb_invalidate(u64 p,u64 va,bool full){(void)p;(void)va;(void)full;assert(vm_locked);tlbs++;}
static u64 read_cr3(void){return 0;}
static void panic(const char*s){fprintf(stderr,"%s\n",s);abort();}
'''
    code += '\n'.join(re.findall(r'^#define PTE_.*$', mm, re.M))+'\n'
    code += 'static inline u64 *table_at(u64 phys){return phys_to_virt(phys & PTE_ADDR);}\n'
    # Pointer-return helpers use the same whole-function extraction delimiter.
    for name in ['walk', 'map_locked', 'vmm_map', 'translate_locked', 'vmm_translate', 'mapped_locked', 'vmm_is_mapped',
                 'user_leaf', 'vmm_unmap_user_page', 'vmm_unmap_borrowed_page',
                 'free_level', 'vmm_destroy_address_space']:
        if name in ('walk', 'user_leaf'):
            start = vmm.index('static u64 *'+name+'(')
            code += vmm[start:vmm.index('\n}', start)+2]+'\n'
        else:
            code += function(vmm, name)+'\n'
    code += r'''
enum{E_INVAL=22,E_NODEV=19,E_BUSY=16,E_NOMEM=12};
enum{PROC_UNUSED,PROC_READY,PROC_ZOMBIE};
#define USER_MMAP_BASE 0x60000000ULL
#define USER_TSTACK_BASE 0x00007FF000000000ULL
#define kinfo(...) ((void)0)
typedef struct proc{
    int pid,state;u64 pml4,mmap_next,fb_mapped_va,fb_mapped_span,fb_mapped_phys;
    struct proc *leader;
}proc_t;
static proc_t processes[3];
static proc_t *proc_shared(proc_t*p){return p->leader?p->leader:p;}
static bool proc_pid_alive(int id){
    for(int i=0;i<3;i++)if(processes[i].pid==id)
        return processes[i].state!=PROC_UNUSED&&processes[i].state!=PROC_ZOMBIE;
    return false;
}
typedef struct{
    u64 base,size;u32 width,height,pitch,bpp;
    unsigned char red_shift,red_bits,green_shift,green_bits,blue_shift,blue_bits;
    unsigned short pad;
}kboot_framebuffer;
typedef struct{u64 address;u32 width,height,pitch,bpp,red_shift,green_shift,blue_shift,size;}kframebuffer_t;
static struct{kboot_framebuffer fb;}g_boot;
static bool native=true,valid_user=true,guard_busy,guard_held;
static proc_t *mock_current;
static bool metadata_held,metadata_busy,copy_failure;
static proc_t *proc_current(void){return mock_current;}
static bool proc_vm_begin(proc_t **token){
    assert(!metadata_held&&!guard_held);*token=NULL;
    if(metadata_busy)return false;metadata_held=true;*token=mock_current;return true;
}
static void proc_vm_end(proc_t *token){assert(metadata_held&&!guard_held&&token==mock_current);metadata_held=false;}
static unsigned releases,guard_ends,map_calls,fail_map;
static int fb_owner_pid;
static bool nvkms_kapi_runtime_selected(void){return native;}
static bool user_range_ok(u64 a,size_t n,bool write){
    (void)n;(void)write;return valid_user&&a&&a<UINT64_MAX-4096;
}
static bool user_copy(void*buffer,u64 address,size_t bytes,bool write){
    assert(write&&metadata_held&&guard_held);
    if(copy_failure)return false;memcpy((void*)address,buffer,bytes);return true;
}
static void console_release_framebuffer(void){releases++;}
static bool nv_render_try_begin(void){if(guard_busy)return false;assert(!guard_held);guard_held=true;return true;}
static void nv_render_end(void){assert(guard_held);guard_held=false;guard_ends++;}
static bool inject_map(u64 p,u64 v,u64 a,u64 f){
    if(++map_calls==fail_map)return false;return vmm_map(p,v,a,f);
}
#define vmm_map inject_map
'''
    code += function(syscall, 'framebuffer_map_locked')+'\n'
    code += function(syscall, 'framebuffer_map')+'\n#undef vmm_map\n'
    code += r'''
static long map_as(proc_t *p,u64 address){
    mock_current=p;long result=framebuffer_map(p,address);
    assert(!metadata_held&&!guard_held);return result;
}
#define framebuffer_map map_as
static void reset(void){
    memset(ram,0,sizeof ram);memset(allocated,0,sizeof allocated);memset(freed,0,sizeof freed);
    memset(processes,0,sizeof processes);alloc_at=fail_alloc=tlbs=0;kernel_pml4=0;
    for(unsigned i=300;i<320;i++)allocated[i]=true; /* display owner */
    for(int i=0;i<2;i++){
        processes[i].pid=i+1;processes[i].state=PROC_READY;
        processes[i].pml4=pmm_alloc_zeroed();processes[i].mmap_next=USER_MMAP_BASE;
    }
    processes[2].pid=3;processes[2].leader=&processes[0];processes[2].pml4=processes[0].pml4;
    g_boot.fb=(kboot_framebuffer){.base=300*4096,.size=3*4096,.width=64,.height=48,.pitch=256,.bpp=32,.red_shift=16,.green_shift=8};
    native=valid_user=true;guard_busy=guard_held=false;fb_owner_pid=0;
    releases=guard_ends=map_calls=fail_map=0;
    metadata_held=metadata_busy=copy_failure=vm_locked=false;mock_current=NULL;
}
static void alive(void){for(unsigned i=300;i<320;i++)assert(allocated[i]&&!freed[i]);}
static void ownership(void){
    reset();u64 p=processes[0].pml4,v=USER_MMAP_BASE,a=pmm_alloc_zeroed();
    assert(vmm_map(p,v,a,PTE_U|PTE_W));
    assert(vmm_map(p,v+4096,300*4096,PTE_U|PTE_W|PTE_BORROWED));
    assert(!vmm_unmap_borrowed_page(p,v,a));
    assert(!vmm_unmap_borrowed_page(p,v+4096,301*4096));
    assert(vmm_unmap_user_page(p,v)&&freed[a/4096]==1);
    assert(!vmm_unmap_user_page(p,v));
    assert(vmm_unmap_user_page(p,v+4096));alive();
    assert(vmm_map(p,v+4096,300*4096,PTE_U|PTE_W|PTE_BORROWED));
    assert(vmm_unmap_borrowed_page(p,v+4096,300*4096));alive();
    a=pmm_alloc_zeroed();assert(vmm_map(p,v,a,PTE_U|PTE_W));
    assert(vmm_map(p,v+4096,300*4096,PTE_U|PTE_BORROWED));
    u64 table_count=0;for(int i=1;i<256;i++)if(allocated[i])table_count++;
    assert(table_count>=6);unsigned before=freed[a/4096];
    vmm_destroy_address_space(p);alive();assert(freed[a/4096]==before+1);
    for(int i=1;i<256;i++)assert(!allocated[i] || i*4096ULL==processes[1].pml4);
    /* Huge and supervisor leaves must never be freed by a 4 KiB unmap. */
    reset();p=processes[0].pml4;
    assert(vmm_map(p,v,300*4096,PTE_U|PTE_BORROWED));
    u64 *pdpt=table_at(table_at(p)[0]),*pd=table_at(pdpt[1]);
    pd[(v>>21)&511]=(512*4096)|PTE_U|PTE_P|PTE_PS|PTE_BORROWED;
    allocated[512]=true;
    assert(!vmm_unmap_user_page(p,v));assert(!vmm_unmap_borrowed_page(p,v,512*4096));
    vmm_destroy_address_space(p);assert(allocated[512]&&!freed[512]);alive();
    reset();p=processes[0].pml4;
    assert(vmm_map(p,v,300*4096,PTE_BORROWED));assert(!vmm_unmap_user_page(p,v));
    assert(!vmm_unmap_user_page(p,1ULL<<47));alive();
    assert(vmm_map(p,v,0,PTE_U|PTE_BORROWED));assert(vmm_is_mapped(p,v));
    assert(vmm_unmap_borrowed_page(p,v,0));assert(!vmm_is_mapped(p,v));
}
static void mapping(void){
    kframebuffer_t info;
    for(unsigned failure=1;failure<=3;failure++){
        reset();proc_t*m=&processes[0];
        assert(framebuffer_map(m,(u64)&info)==0);u64 oldva=info.address;
        proc_t old=*m;int oldowner=fb_owner_pid;unsigned oldrelease=releases;
        g_boot.fb.base=304*4096;map_calls=0;fail_map=failure;
        kframebuffer_t unchanged;memset(&info,0xa5,sizeof info);unchanged=info;
        assert(framebuffer_map(m,(u64)&info)==-E_NOMEM);
        assert(!memcmp(m,&old,sizeof old)&&!memcmp(&info,&unchanged,sizeof info));
        assert(fb_owner_pid==oldowner&&releases==oldrelease&&!guard_held);
        for(u64 off=0;off<3*4096;off+=4096){
            assert(vmm_translate(m->pml4,oldva+off)==300*4096+off);
            assert(!vmm_is_mapped(m->pml4,old.mmap_next+off));
        }
        alive();
    }
    /* Real page-table allocator fails at each intermediate allocation. */
    for(unsigned failure=1;failure<=3;failure++){
        reset();proc_t*m=&processes[0];proc_t old=*m;fail_alloc=failure;alloc_at=0;
        assert(framebuffer_map(m,(u64)&info)==-E_NOMEM);
        assert(!memcmp(m,&old,sizeof old)&&!fb_owner_pid&&!releases);
        assert(!vmm_is_mapped(m->pml4,old.mmap_next));alive();
        vmm_destroy_address_space(m->pml4);alive();
    }
    reset();proc_t*m=&processes[0];
    assert(framebuffer_map(m,(u64)&info)==0);
    u64 oldva=info.address;assert(info.size==g_boot.fb.size);
    assert((*user_leaf(m->pml4,oldva)&(PTE_BORROWED|PTE_PWT))==PTE_BORROWED);
    assert(framebuffer_map(m,oldva)==-E_INVAL); /* result aliases retired view */
    assert(vmm_unmap_user_page(m->pml4,oldva+4096));
    u64 owned=pmm_alloc_zeroed();assert(vmm_map(m->pml4,oldva+4096,owned,PTE_U|PTE_W));
    g_boot.fb.base=304*4096;native=false;
    assert(framebuffer_map(&processes[2],(u64)&info)==0); /* thread uses leader */
    assert(!processes[2].fb_mapped_va&&m->fb_mapped_va==info.address);
    assert(!vmm_is_mapped(m->pml4,oldva));
    assert(vmm_translate(m->pml4,oldva+4096)==owned&&allocated[owned/4096]);
    assert((*user_leaf(m->pml4,info.address)&(PTE_BORROWED|PTE_PWT))==(PTE_BORROWED|PTE_PWT));
    assert(framebuffer_map(&processes[1],(u64)&info)==-E_BUSY);
    fb_owner_pid=0; /* release keeps mapping: another address space may acquire */
    assert(framebuffer_map(&processes[1],(u64)&info)==0);
    assert(vmm_is_mapped(m->pml4,m->fb_mapped_va));alive();
    vmm_destroy_address_space(m->pml4);vmm_destroy_address_space(processes[1].pml4);alive();
    reset();m=&processes[0];g_boot.fb.base+=13;
    assert(framebuffer_map(m,(u64)&info)==0);
    assert((info.address&PAGE_MASK)==13&&vmm_translate(m->pml4,info.address)==g_boot.fb.base);
    assert(m->fb_mapped_span==4*4096);alive();
    reset();m=&processes[0];
    assert(vmm_map(m->pml4,m->mmap_next,300*4096,PTE_U|PTE_BORROWED));
    assert(framebuffer_map(m,(u64)&info)==-E_BUSY&&!fb_owner_pid);
    assert(vmm_translate(m->pml4,m->mmap_next)==300*4096);
    for(int bad=0;bad<10;bad++){
        reset();m=&processes[0];
        if(bad==0)g_boot.fb.size=UINT64_MAX;if(bad==1)g_boot.fb.base=UINT64_MAX-4095;
        if(bad==2)g_boot.fb.pitch=1;if(bad==3)g_boot.fb.width=UINT32_MAX;
        if(bad==4)g_boot.fb.bpp=24;if(bad==5)g_boot.fb.size=1;
        if(bad==6)m->mmap_next=UINT64_MAX-4095;
        if(bad==7)m->mmap_next=USER_TSTACK_BASE-4096;
        if(bad==8)m->mmap_next++;if(bad==9)valid_user=false;
        assert(framebuffer_map(m,(u64)&info)<0&&!map_calls&&!releases&&!guard_held);alive();
    }
    reset();guard_busy=true;assert(framebuffer_map(&processes[0],(u64)&info)==-E_BUSY&&!guard_ends);
    reset();metadata_busy=true;assert(framebuffer_map(&processes[0],(u64)&info)==-E_BUSY&&!map_calls&&!guard_ends);
    reset();m=&processes[0];assert(framebuffer_map(m,(u64)&info)==0);
    proc_t old=*m;oldva=info.address;copy_failure=true;g_boot.fb.base=304*4096;
    assert(framebuffer_map(m,(u64)&info)==-E_INVAL&&!memcmp(m,&old,sizeof old));
    assert(info.address==oldva&&vmm_translate(m->pml4,oldva)==300*4096);
    assert(!vmm_is_mapped(m->pml4,old.mmap_next));alive();
}
int main(void){ownership();mapping();puts("PASS: actual VMM borrowed ownership, exit/unmap, huge/supervisor protection, per-process framebuffer mapping, every page/allocator failure, alias/overflow/collision checks");}
'''
    run_test(code, 'framebuffer_mapping')
    # Both native munmap and Linux's delegated path use leaf ownership.
    memory = (ROOT / 'kernel/process_memory.h').read_text()
    munmap = memory.split('case SYS_MUNMAP: {', 1)[1].split('default:', 1)[0]
    assert 'vmm_unmap_user_range' in munmap and 'pmm_free_page' not in munmap
    assert 'PTE_BORROWED' in mm
    assert 'static u64 fb_mapped_va' not in syscall


if __name__ == '__main__':
    main()
