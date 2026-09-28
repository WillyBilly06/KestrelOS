#!/usr/bin/env python3
"""Production TLS/stat syscall paths, with privileged and filesystem edges mocked.

No codec execution, VM, or GPU model. Copy failure after a blocking metadata
operation models a sibling unmapping its result; it must not become a raw store.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test

abi = (ROOT / 'kernel/linux_abi.c').read_text()
native = (ROOT / 'kernel/syscall.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;typedef int32_t s32;typedef uint64_t u64;typedef int64_t s64;
enum{E_PERM=1,E_NOENT=2,E_IO=5,E_BADF=9,E_FAULT=14,E_INVAL=22,E_NOSYS=38,
     E_NAMETOOLONG=36,VFS_PATH_MAX=256,VN_FILE=1,VN_DIR=2,VN_CHR=3,VN_BLK=4,
     SYS_STAT=1000,THREAD_SETGS=1001};
#define MSR_FS_BASE 0xc0000100u
#define MSR_GS_BASE 0xc0000101u
typedef struct{u64 fsbase,gsbase;bool is_kernel;}proc_t;
static proc_t thread_a,thread_b,*current=&thread_a;
static bool irq_enabled=true,online=true,mapped=true,drop_on_stat,drop_on_close;
static unsigned writes,copies,closes,refs,lookups;
static u32 last_msr;static u64 last_base;
static int fs_error,resolve_error;
static bool irq_save(void){bool old=irq_enabled;irq_enabled=false;return old;}
static void irq_restore(bool old){if(old)irq_enabled=true;}
static bool scheduler_local_online(void){return online;}
static void wrmsr(u32 msr,u64 base){
    assert(!irq_enabled&&base<(1ull<<47));
    assert(msr==MSR_FS_BASE||msr==MSR_GS_BASE);writes++;last_msr=msr;last_base=base;
}
typedef struct{u32 type,mode;u64 size,mtime;}kstat_t;
typedef kstat_t vstat_t;
typedef struct vnode vnode_t;
typedef struct{int(*stat)(vnode_t*,vstat_t*);}vops_t;
struct vnode{u32 type;u64 size;vops_t*ops;};
typedef struct{vnode_t*vn;}file_t;
static int stat_callback(vnode_t*vn,vstat_t*st){
    (void)vn;st->mtime=12345;st->mode=0644;
    if(drop_on_stat)mapped=false;
    return fs_error;
}
static vops_t ops={stat_callback};static vnode_t node={VN_FILE,123456,&ops};
static file_t file={&node};
static int vfs_resolve(const char*path,vnode_t**out){
    lookups++;assert(!strcmp(path,"/sample"));
    if(resolve_error)return resolve_error;
    refs++;*out=&node;return 0;
}
static void vnode_unref(vnode_t*vn){assert(vn==&node&&refs);refs--;}
static bool user_range_ok(u64 address,size_t bytes,bool out){
    (void)out;return mapped&&address>=4096&&address<(1ull<<47)&&bytes<4096;
}
static bool user_copy(void*buffer,u64 address,size_t bytes,bool out){
    copies++;if(!user_range_ok(address,bytes,out))return false;
    if(out)memcpy((void*)(uintptr_t)address,buffer,bytes);
    else memcpy(buffer,(void*)(uintptr_t)address,bytes);
    return true;
}
#define syscall_user_ok user_range_ok
#define syscall_user_copy user_copy
static void proc_resolve_path(proc_t*p,const char*raw,char*full,size_t cap){
    (void)p;assert(strlen(raw)<cap);memcpy(full,raw,strlen(raw)+1);
}
static file_t*proc_fd_acquire(proc_t*p,int fd){(void)p;if(fd!=3)return NULL;refs++;return &file;}
static void vfs_close(file_t*f){assert(f==&file&&refs);refs--;closes++;if(drop_on_close)mapped=false;}
'''
code += '\n'.join(re.findall(r'^#define (?:ARCH_\w+|L_arch_prctl|L_stat|L_lstat|L_newfstatat|L_fstat|L_access|L_AT_FDCWD|L_S_IF\w+)\s+[^\n]+', abi, re.M))+'\n'
start = abi.index('typedef struct {\n    u64 st_dev;')
code += abi[start:abi.index('} linux_stat_t;',start)+len('} linux_stat_t;')]+'\n'
code += function((ROOT/'kernel/sched.c').read_text(),'proc_thread_set_base')+'\n'
code += function(native,'copy_user_string')+'\n'
code += function((ROOT/'kernel/vfs.c').read_text(),'locked_vfs_stat')+'\n'
code += '#define vfs_stat locked_vfs_stat\n'
code += function(native,'syscall_stat_snapshot')+'\n'
code += function(abi,'fill_stat')+'\n'
code += 'static s64 dispatch(u64 nr,u64 a0,u64 a1,u64 a2){proc_t*p=current;switch(nr){\n'
start=abi.index('    case L_arch_prctl:')
code += abi[start:abi.index('    /* Snapshot vector metadata',start)]
start=abi.index('    case L_stat:')
code += abi[start:abi.index('    /* Sleeping,',start)]
start=abi.index('    case L_access:')
code += abi[start:abi.index('    /* F_GETFL',start)]
start=native.index('    case SYS_STAT:')
code += native[start:native.index('    case SYS_READDIR:',start)]
start=native.index('        case THREAD_SETGS:')
code += native[start:native.index('        case THREAD_GETTID:',start)]
code += 'default:return -E_NOSYS;}}\n'
code += r'''
int main(void){
    assert(!dispatch(L_arch_prctl,ARCH_SET_FS,0x12345,0));
    assert(thread_a.fsbase==0x12345&&last_msr==MSR_FS_BASE&&irq_enabled);
    assert(!dispatch(THREAD_SETGS,0,0x98765,0));
    assert(thread_a.gsbase==0x98765&&last_msr==MSR_GS_BASE);
    unsigned before=writes;
    u64 invalid[]={1ull<<47,0x0000800000000001ull,0xffff800000000000ull,UINT64_MAX};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
        assert(dispatch(L_arch_prctl,ARCH_SET_FS,invalid[i],0)==-E_PERM);
        assert(dispatch(L_arch_prctl,ARCH_SET_GS,invalid[i],0)==-E_PERM);
        assert(dispatch(THREAD_SETGS,0,invalid[i],0)==-E_PERM);
    }
    assert(writes==before&&thread_a.fsbase==0x12345&&thread_a.gsbase==0x98765);
    current=&thread_b;irq_enabled=false;
    assert(!dispatch(L_arch_prctl,ARCH_SET_FS,(1ull<<47)-1,0)&&!irq_enabled);
    assert(thread_b.fsbase==(1ull<<47)-1&&thread_a.fsbase==0x12345);
    assert(!dispatch(L_arch_prctl,ARCH_SET_FS,0,0)&&thread_b.fsbase==0);
    current=&thread_a;irq_enabled=true;u64 output=0;
    assert(!dispatch(L_arch_prctl,ARCH_GET_FS,(u64)&output,0)&&output==0x12345);
    assert(!dispatch(L_arch_prctl,ARCH_GET_GS,(u64)&output,0)&&output==0x98765);
    mapped=false;assert(dispatch(L_arch_prctl,ARCH_GET_FS,(u64)&output,0)==-E_FAULT);mapped=true;
    assert(dispatch(L_arch_prctl,0,0,0)==-E_INVAL);
    before=writes;thread_a.is_kernel=true;
    assert(dispatch(L_arch_prctl,ARCH_SET_FS,1,0)==-E_INVAL&&writes==before);thread_a.is_kernel=false;
    online=false;assert(proc_thread_set_base(true,1)==-E_INVAL&&irq_enabled);online=true;
    current=NULL;assert(proc_thread_set_base(true,1)==-E_INVAL&&irq_enabled);current=&thread_a;

    const char path[]="/sample";linux_stat_t st;memset(&st,0xa5,sizeof st);
    assert(!dispatch(L_stat,(u64)path,(u64)&st,0)&&st.st_size==123456&&st.mtime_sec==12345);
    assert(st.st_blocks==242&&st.st_mode==(L_S_IFREG|0644)&&st.reserved[0]==0&&!refs);
    assert(!dispatch(L_lstat,(u64)path,(u64)&st,0));
    assert(!dispatch(L_newfstatat,(u64)(s64)L_AT_FDCWD,(u64)path,(u64)&st));
    assert(dispatch(L_newfstatat,3,(u64)path,(u64)&st)==-E_NOSYS);
    assert(!dispatch(L_access,(u64)path,0,0));
    kstat_t ks={0};assert(!dispatch(SYS_STAT,(u64)path,(u64)&ks,0)&&ks.size==123456);
    fs_error=-E_IO;assert(dispatch(L_stat,(u64)path,(u64)&st,0)==-E_IO&&!refs);
    assert(dispatch(SYS_STAT,(u64)path,(u64)&ks,0)==-E_IO&&!refs);fs_error=0;
    resolve_error=-E_NOENT;assert(dispatch(L_access,(u64)path,0,0)==-E_NOENT&&!refs);resolve_error=0;
    memset(&st,0xa5,sizeof st);drop_on_stat=true;
    assert(dispatch(L_stat,(u64)path,(u64)&st,0)==-E_FAULT&&!refs);
    for(unsigned i=0;i<sizeof st;i++)assert(((unsigned char*)&st)[i]==0xa5);
    mapped=true;ks.size=99;assert(dispatch(SYS_STAT,(u64)path,(u64)&ks,0)==-E_INVAL&&ks.size==99);
    drop_on_stat=false;mapped=true;
    assert(!dispatch(L_fstat,3,(u64)&st,0)&&st.st_size==123456&&!refs&&closes==1);
    assert(dispatch(L_fstat,4,(u64)&st,0)==-E_BADF&&!refs);
    drop_on_close=true;assert(dispatch(L_fstat,3,(u64)&st,0)==-E_FAULT&&!refs&&closes==2);
    puts("PASS production TLS/metadata paths: canonical bases, IRQ-bounded MSR/bookkeeping, per-thread isolation, zero TLS, protected GET, kernel snapshots, native/Linux stat, post-wait unmap, retained fstat, filesystem errors; MSR/VMM/filesystem edges mocked");
}
'''
if __name__ == '__main__':
    run_test(code,'thread-runtime-metadata')
