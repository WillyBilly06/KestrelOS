#!/usr/bin/env python3
"""Production TID word validation/store/key against synthetic real page tables.

CR3 and physical-table access are host adapters; no privileged CPU execution.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test

vmm = (ROOT / 'kernel/vmm.c').read_text()
mm = (ROOT / 'kernel/mm.h').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;typedef uint64_t u64;
#define PAGE_SIZE 4096ull
#define PAGE_MASK 4095ull
#define PAGE_ALIGN_DOWN(x) ((x)&~PAGE_MASK)
static u64 tables[4][512],loaded_cr3;
__declspec(align(4096)) static u32 user_page[1024];
static bool locked,irq_on=true;
static unsigned locks;
static bool vmm_lock(void){assert(!locked);bool old=irq_on;irq_on=false;locked=true;locks++;return old;}
static void vmm_unlock(bool irq){assert(locked&&!irq_on);locked=false;irq_on=irq;}
static u64 read_cr3(void){assert(locked);return loaded_cr3;}
'''
code += '\n'.join(re.findall(r'^#define PTE_.*$',mm,re.M))+'\n'
code += r'''
static u64*table_at(u64 pa){assert(locked);pa&=PTE_ADDR;assert(pa>=4096&&pa<=4*4096);return tables[pa/4096-1];}
'''
for name in ('translate_locked','user_range_locked','vmm_user_store_word_key'):
    code += function(vmm,name)+'\n'
code += r'''
static unsigned index_at(u64 va,unsigned shift){return (unsigned)((va>>shift)&511);}
static void reset(u64 va){
    memset(tables,0,sizeof tables);loaded_cr3=4096;locks=0;irq_on=true;locked=false;user_page[0]=999;
    for(unsigned level=0;level<3;level++)tables[level][index_at(va,39-level*9)]=
        (level+2)*4096|PTE_P|PTE_U|PTE_W;
    tables[3][index_at(va,12)]=0x8000|PTE_P|PTE_U|PTE_W;
}
int main(void){
    u64 va=(u64)(uintptr_t)user_page,key=UINT64_MAX;
    reset(va);assert(vmm_user_store_word_key(4096,va,0,&key)&&!user_page[0]&&key==0x8000&&locks==1&&irq_on&&!locked);
    for(unsigned level=0;level<4;level++)for(unsigned permission=0;permission<2;permission++){
        reset(va);tables[level][index_at(va,39-level*9)]&=~(permission?PTE_W:PTE_U);key=UINT64_MAX;
        assert(!vmm_user_store_word_key(4096,va,0,&key)&&key==UINT64_MAX&&user_page[0]==999&&irq_on&&!locked);
    }
    reset(va);tables[3][index_at(va,12)]=PTE_P|PTE_U|PTE_W;
    assert(vmm_user_store_word_key(4096,va,0,&key)&&key==0); // physical zero is a valid key
    reset(va);tables[3][index_at(va,12)]&=~PTE_P;
    assert(!vmm_user_store_word_key(4096,va,0,&key)&&user_page[0]==999);
    reset(va);loaded_cr3=8192;assert(!vmm_user_store_word_key(4096,va,0,&key)&&user_page[0]==999);
    reset(va);assert(!vmm_user_store_word_key(4096,va+1,0,&key)&&!locks);
    assert(!vmm_user_store_word_key(4096,va,0,NULL)&&!locks);
    assert(!vmm_user_store_word_key(4096,0,0,&key));
    assert(!vmm_user_store_word_key(4096,1ull<<47,0,&key));
    assert(!vmm_user_store_word_key(4096,UINT64_MAX-3,0,&key));
    reset(va);tables[0][index_at(va,39)]|=PTE_PS;
    assert(!vmm_user_store_word_key(4096,va,0,&key)); // invalid huge PML4
    reset(va);tables[2][index_at(va,21)]=0x200000|PTE_P|PTE_U|PTE_W|PTE_PS;
    assert(vmm_user_store_word_key(4096,va,0,&key)&&key==0x200000+(va&0x1fffff));
    reset(va);tables[1][index_at(va,30)]=0x40000000|PTE_P|PTE_U|PTE_W|PTE_PS;
    assert(vmm_user_store_word_key(4096,va,0,&key)&&key==0x40000000+(va&0x3fffffff));
    puts("PASS production TID word/page-table helper: every-level user/write permissions, current CR3, aligned/canonical/present checks, physical-zero and huge-page keys, one-lock release store; CR3/physical table access mocked");
}
'''
if __name__ == '__main__':
    run_test(code, 'vmm-tid-word')
