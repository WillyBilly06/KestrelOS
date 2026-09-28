#!/usr/bin/env python3
"""Production staged file I/O + native/Linux dispatch fragments on host threads.

VM mappings, callbacks and IRQs are mocked. Actual VMM permission/copy behavior
is covered separately; no native AP/driver/DMA or hardware test is performed.
"""
from test_process_fds import code as fd_code
from test_gpu_stable_candidate import ROOT, run_test

code = fd_code.split('static filesystem_t memory_fs=')[0]
start = code.index('static bool vmm_user_copy(')
end = code.index('static proc_t *proc_shared', start)
code = code[:start] + 'static bool vmm_user_copy(u64,void*,u64,size_t,bool);\n' + code[end:]
code = code.replace('struct proc *leader;file_t', 'struct proc *leader;bool is_kernel;file_t')
code = code.replace('static bool copy_fails;', '')
code += r'''
typedef uint8_t u8;
enum{E_NOMEM=12,E_IO=5};
#define PAGE_SIZE 4096u
#define VMM_USER_COPY_MAX 16384u
#define VFS_IOV_MAX 1024u
typedef struct {u64 base,len;}vfs_iovec_t;
static volatile LONG allocations,attempts;
static LONG fail_on;
static void *kmalloc(size_t n){
    LONG call=InterlockedIncrement(&attempts);if(call==fail_on)return NULL;
    void*p=malloc(n);if(p)InterlockedIncrement(&allocations);return p;
}
static void kfree(void*p){if(p){InterlockedDecrement(&allocations);free(p);}}
typedef struct{u64 base;size_t len;bool valid,writable;}region_t;
static region_t regions[16];static unsigned region_count;
static SRWLOCK vm_lock=SRWLOCK_INIT;
static _Thread_local bool vm_owned;
static void map(void*p,size_t n){assert(region_count<16);regions[region_count++]=(region_t){(u64)(uintptr_t)p,n,true,true};}
static bool mapped(u64 a,size_t n,bool writable){
    for(unsigned i=0;i<region_count;i++){
        region_t*r=&regions[i];
        if(r->valid&&(!writable||r->writable)&&a>=r->base&&a-r->base<=r->len&&n<=r->len-(a-r->base))return true;
    }
    return !n;
}
static bool vmm_user_range_ok(u64 root,u64 a,size_t n,bool writable){
    assert(root==0x1000);AcquireSRWLockExclusive(&vm_lock);bool ok=mapped(a,n,writable);
    ReleaseSRWLockExclusive(&vm_lock);return ok;
}
static bool vmm_user_copy(u64 root,void*buffer,u64 a,size_t n,bool out){
    assert(root==0x1000&&buffer&&n<=VMM_USER_COPY_MAX&&!vm_owned);
    AcquireSRWLockExclusive(&vm_lock);vm_owned=true;bool ok=mapped(a,n,out);
    if(ok){if(out)memcpy((void*)(uintptr_t)a,buffer,n);else memcpy(buffer,(void*)(uintptr_t)a,n);}
    vm_owned=false;ReleaseSRWLockExclusive(&vm_lock);return ok;
}
'''
code += (ROOT / 'kernel/vfs_user_io.h').read_text()
code += r'''
static proc_t process={.pml4=0x1000};
static bool user_range_ok(u64 a,size_t n,bool write){return process.is_kernel||vmm_user_range_ok(process.pml4,a,n,write);}
static bool syscall_user_copy(void*b,u64 a,size_t n,bool out){return vmm_user_copy(process.pml4,b,a,n,out);}
typedef vfs_iovec_t linux_iovec_t;
enum{SYS_WRITE=1,SYS_READ=2,L_readv=19,L_writev=20};
'''
native = (ROOT / 'kernel/syscall.c').read_text()
native = native[native.index('    case SYS_WRITE: {'):native.index('    case SYS_OPEN:', native.index('    case SYS_WRITE: {'))]
linux = (ROOT / 'kernel/linux_abi.c').read_text()
linux = linux[linux.index('    case L_writev:'):linux.index('    /* The three that hand back a stat.')]
code += 'static s64 native_io(proc_t*p,u64 nr,u64 a0,u64 a1,u64 a2){switch(nr){\n' + native + '\ndefault:return -E_INVAL;}}\n'
code += 'static s64 linux_io(proc_t*p,u64 nr,u64 a0,u64 a1,u64 a2){switch(nr){\n' + linux + '\ndefault:return -E_INVAL;}}\n'
code += r'''
static unsigned char disk[2000000],input[40000],output[40000];
static filesystem_t memory_fs={true};
static unsigned calls;
static int fail_driver_at,invalidate_at,invalidate_region;
static bool oversize,mutate_input;
static vfs_iovec_t *mutate_vector;
static size_t short_at;
static ssize_t_k read_driver(vnode_t*vn,void*b,size_t n,u64 off){
    assert(!vm_owned&&irq_enabled); // no VM lock held over a potentially blocking driver
    assert(!mapped((u64)(uintptr_t)b,n,false));calls++;
    if((int)calls==fail_driver_at)return -E_IO;
    if(off>=vn->size)return 0;if(n>vn->size-off)n=(size_t)(vn->size-off);
    if(short_at&&n>short_at)n=short_at;
    memcpy(b,disk+off,n);
    if((int)calls==invalidate_at){AcquireSRWLockExclusive(&vm_lock);regions[invalidate_region].valid=false;ReleaseSRWLockExclusive(&vm_lock);}
    return n+(oversize?1:0);
}
static ssize_t_k write_driver(vnode_t*vn,const void*b,size_t n,u64 off){
    assert(!vm_owned&&irq_enabled);assert(!mapped((u64)(uintptr_t)b,n,false));calls++;
    if((int)calls==fail_driver_at)return -E_IO;
    if(short_at&&n>short_at)n=short_at;
    if(mutate_input){memset(input,'Z',sizeof input);mutate_input=false;}
    if(mutate_vector){mutate_vector[1]=(vfs_iovec_t){UINT64_MAX,UINT64_MAX};mutate_vector=NULL;}
    assert(off+n<=sizeof disk);memcpy(disk+off,b,n);if(off+n>vn->size)vn->size=off+n;
    SwitchToThread();return n+(oversize?1:0);
}
static vnode_ops_t ops={.read=read_driver,.write=write_driver};
static vnode_t vn;
static file_t *f;
static void reset(void){
    if(f){proc_fd_close_all(&process);assert(!f->refs);f=NULL;}
    assert(!allocations);region_count=calls=0;fail_driver_at=invalidate_at=-1;fail_on=0;attempts=0;
    oversize=mutate_input=false;mutate_vector=NULL;short_at=0;process.is_kernel=false;
    vn=(vnode_t){.type=VN_FILE,.refs=1,.fs=&memory_fs,.ops=&ops};
    assert(!vfs_open_vnode(&vn,O_RDWR,&f)&&proc_fd_alloc(&process,f)==0);
    map(input,sizeof input);map(output,sizeof output);
    memset(input,'A',sizeof input);memset(output,0xcc,sizeof output);memset(disk,0,sizeof disk);
}
static unsigned char thread_buffers[8][20000];
static DWORD WINAPI writer(void*arg){
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<10;i++)assert(vfs_user_io(f,0x1000,(u64)(uintptr_t)thread_buffers[id],20000,true)==20000);
    return 0;
}
int main(void){
    reset();assert(native_io(&process,SYS_WRITE,0,(u64)(uintptr_t)input,sizeof input)==sizeof input);
    assert(calls==3&&f->pos==sizeof input&&f->refs==1&&!allocations&&!f->io_busy);
    f->pos=0;calls=0;assert(native_io(&process,SYS_READ,0,(u64)(uintptr_t)output,sizeof output)==sizeof output);
    assert(!memcmp(input,output,sizeof input)&&calls==3&&f->refs==1);
    reset();mutate_input=true;assert(vfs_user_io(f,0x1000,(u64)(uintptr_t)input,30,true)==30);
    for(unsigned i=0;i<30;i++)assert(disk[i]=='A'&&input[i]=='Z'); // callback sees private snapshot
    reset();fail_on=1;assert(native_io(&process,SYS_WRITE,0,(u64)(uintptr_t)input,10)==-E_NOMEM);
    assert(!calls&&f->refs==1&&!allocations);fail_on=0;
    assert(vfs_user_io(f,0x1000,0,0,true)==0);
    assert(vfs_user_io(f,0x1000,UINT64_MAX,2,true)==-E_INVAL);
    assert(vfs_user_iov(f,0x1000,NULL,1025,true)==-E_INVAL);
    vfs_iovec_t huge={0x1000,UINT64_MAX};assert(vfs_user_iov(f,0x1000,&huge,1,true)==-E_INVAL);

    reset();memset(disk,'B',sizeof output);vn.size=sizeof output;
    invalidate_at=1;invalidate_region=1;
    assert(native_io(&process,SYS_READ,0,(u64)(uintptr_t)output,sizeof output)==-E_INVAL);
    assert(f->pos==0&&f->refs==1&&output[0]==0xcc&&!allocations); // unmapped during driver wait
    reset();memset(disk,'B',sizeof output);vn.size=sizeof output;invalidate_at=2;invalidate_region=1;
    assert(vfs_user_io(f,0x1000,(u64)(uintptr_t)output,sizeof output,false)==16384);
    assert(f->pos==16384&&output[0]=='B'&&output[16384]==0xcc);
    reset();fail_driver_at=2;
    assert(vfs_user_io(f,0x1000,(u64)(uintptr_t)input,sizeof input,true)==16384&&f->pos==16384);
    reset();oversize=true;
    assert(vfs_user_io(f,0x1000,(u64)(uintptr_t)input,20,true)==-E_IO&&f->pos==0);

    reset();linux_iovec_t vectors[3]={{0,0},{(u64)(uintptr_t)input,20000},{(u64)(uintptr_t)(input+20000),20000}};
    map(vectors,sizeof vectors);mutate_vector=vectors;
    assert(linux_io(&process,L_writev,0,(u64)(uintptr_t)vectors,3)==40000);
    assert(calls==3&&f->pos==40000&&disk[39999]=='A'&&f->refs==1&&!allocations);
    reset();vectors[0]=(vfs_iovec_t){(u64)(uintptr_t)input,20};map(vectors,sizeof vectors);
    fail_on=1;assert(linux_io(&process,L_writev,0,(u64)(uintptr_t)vectors,1)==-E_NOMEM&&f->refs==1);
    fail_on=0;regions[2].valid=false;
    assert(linux_io(&process,L_writev,0,(u64)(uintptr_t)vectors,1)==-E_INVAL&&f->refs==1&&!allocations);
    assert(linux_io(&process,L_writev,0,0,0)==0&&f->refs==1);

    reset();vn.type=VN_CHR;vn.size=100;memset(disk,'C',100);
    vectors[0]=(vfs_iovec_t){(u64)(uintptr_t)input,7};vectors[1]=(vfs_iovec_t){(u64)(uintptr_t)output,8};
    assert(vfs_user_iov(f,0x1000,vectors,2,false)==15&&calls==1&&!f->pos&&!f->io_busy);
    for(unsigned i=0;i<7;i++)assert(input[i]=='C');for(unsigned i=0;i<8;i++)assert(output[i]=='C');
    reset();vn.size=100;memset(disk,'D',100);invalidate_at=1;invalidate_region=1;
    assert(vfs_user_iov(f,0x1000,vectors,2,false)==7&&f->pos==7&&input[0]=='D'&&output[0]==0xcc);
    reset();vn.size=100;short_at=3;memset(disk,'S',100);
    assert(vfs_user_io(f,0x1000,(u64)(uintptr_t)output,100,false)==3&&f->pos==3&&calls==1);

    reset();region_count=0;HANDLE threads[8];
    for(unsigned i=0;i<8;i++){memset(thread_buffers[i],i+1,20000);map(thread_buffers[i],20000);}
    for(unsigned i=0;i<8;i++){threads[i]=CreateThread(NULL,0,writer,(void*)(uintptr_t)i,0,NULL);assert(threads[i]);}
    assert(WaitForMultipleObjects(8,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<8;i++)CloseHandle(threads[i]);
    assert(f->pos==1600000&&vn.size==1600000&&!allocations&&!f->io_busy);
    for(unsigned off=0;off<1600000;off+=20000){
        unsigned char value=disk[off];assert(value>=1&&value<=8);
        for(unsigned i=1;i<20000;i++)assert(disk[off+i]==value); // no interleaving between internal chunks
    }
    proc_fd_close_all(&process);assert(!allocations&&!f->refs);
    puts("PASS production user I/O/native+Linux dispatch: bounded staging, immutable vectors, mapping loss during driver wait, prefix/offset/error cleanup, stream scatter, short I/O, 8 threads/80 multi-chunk atomic description writes; VM/driver edges mocked, not native AP validation");
}
'''
if __name__ == '__main__':
    run_test(code, 'vfs-user-io')
