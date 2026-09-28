#!/usr/bin/env python3
"""Production FD table + VFS lifetime/offset code on Windows host threads.

Mocked edges: IRQ/TLB, user copy, vnode driver callbacks. This is not native
AP scheduling, filesystem-driver concurrency, or GPU/codec validation.
"""
import re
from test_gpu_stable_candidate import ROOT, run_test

vfs = (ROOT / 'kernel/vfs.c').read_text()

def extract(name):
    match = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;]*?\)\s*\{', vfs, re.M)
    assert match, name
    depth = 1
    i = match.end()
    while depth:
        depth += (vfs[i] == '{') - (vfs[i] == '}')
        i += 1
    return vfs[match.start():i] + '\n'

code = r'''
#include <windows.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int64_t s64;
typedef int64_t ssize_t_k;
enum { E_BADF=9,E_INVAL=22,E_MFILE=24,E_SPIPE=29,E_NOSYS=38 };
enum { O_RDONLY=0,O_WRONLY=1,O_RDWR=2,O_ACCMODE=3,O_APPEND=0x400 };
enum { VN_FILE=1,VN_CHR=3 };
typedef struct vnode vnode_t;
typedef struct {u32 type;u64 size;u32 mode;u64 mtime;} vstat_t;
typedef struct {bool reentrant;} filesystem_t;
typedef struct {u32 in_bytes,out_bytes;bool required;} vfs_ioctl_shape_t;
typedef struct {
    ssize_t_k (*read)(vnode_t*,void*,size_t,u64);
    ssize_t_k (*write)(vnode_t*,const void*,size_t,u64);
    int (*truncate)(vnode_t*,u64);
    int (*sync)(vnode_t*); void (*release)(vnode_t*);
    int (*stat)(vnode_t*,vstat_t*);int (*ioctl)(vnode_t*,u32,void*);
    int (*ioctl_shape)(vnode_t*,u32,vfs_ioctl_shape_t*);
} vnode_ops_t;
struct vnode {u32 type;u64 size;u32 refs;void *priv;filesystem_t *fs;const vnode_ops_t *ops;};
'''
header = (ROOT / 'kernel/vfs.h').read_text()
start = header.index('typedef struct {', header.index('/* Open file description. */'))
code += header[start:header.index('} file_t;', start) + len('} file_t;')]
code += r'''
#define MAX_FILES 256
#define PROC_MAX_FDS 32
typedef struct proc {struct proc *leader;file_t *fds[PROC_MAX_FDS];u64 pml4;} proc_t;
typedef struct {u32 held;} spinlock_t;
static file_t files[MAX_FILES];
static spinlock_t file_pool_lock;
static _Thread_local bool irq_enabled=true;
static volatile u32 fs_busy;
static bool irq_save(void){bool old=irq_enabled;irq_enabled=false;return old;}
static void spin_unlock_irqrestore(spinlock_t *s,bool irq){
    __atomic_store_n(&s->held,0,__ATOMIC_RELEASE);irq_enabled=irq;
}
static void smp_tlb_poll(void){}
static void sched_yield(void){assert(irq_enabled);SwitchToThread();}
static void panic(const char *s){fprintf(stderr,"%s\n",s);abort();}
#define kerr(...) ((void)0)
static bool copy_fails;
static bool vmm_user_copy(u64 pml4,void *from,u64 to,size_t n,bool out){
    assert(pml4==0x1000&&n==8&&out&&!irq_enabled);
    if(copy_fails)return false;memcpy((void*)(uintptr_t)to,from,n);return true;
}
static proc_t *proc_shared(proc_t *p){return p->leader?p->leader:p;}
'''
for name in ('fs_enter','fs_leave','files_lock','files_unlock','vfs_file_ref',
             'vnode_ref','vnode_unref','vfs_open_vnode','vfs_close',
             'file_position_enter','file_position_leave','locked_vfs_read',
             'locked_vfs_write','locked_vfs_seek','locked_vfs_truncate',
             'vfs_read','vfs_write','vfs_seek','vfs_truncate'):
    code += extract(name)
code += (ROOT / 'kernel/process_fds.h').read_text()
code += r'''
static filesystem_t memory_fs={true};
static volatile LONG releases,syncs;
static file_t *retiring,*callback_file;
static vnode_t callback_vnode;
static int driver_active;
static HANDLE read_started,read_finish;
static int sync_driver(vnode_t *vn){
    (void)vn;assert(irq_enabled&&fs_busy&&!file_pool_lock.held&&!descriptor_lock.held);
    InterlockedIncrement(&syncs);
    if(retiring){
        assert(retiring->in_use&&!retiring->refs&&!vfs_file_ref(retiring));
        assert(!vfs_open_vnode(&callback_vnode,O_RDWR,&callback_file));
        assert(callback_file!=retiring);retiring=NULL;
    }
    return 0;
}
static void release_driver(vnode_t *vn){assert(!vn->refs);InterlockedIncrement(&releases);}
static ssize_t_k write_driver(vnode_t *vn,const void *buf,size_t n,u64 off){
    (void)buf;assert(irq_enabled);
    if(vn->type==VN_CHR)return n;
    assert(!driver_active++);assert(off==vn->size);SwitchToThread();
    vn->size+=n;assert(driver_active--==1);return n;
}
static ssize_t_k read_driver(vnode_t *vn,void *buf,size_t n,u64 off){
    (void)buf;(void)off;
    if(vn->type==VN_CHR){SetEvent(read_started);assert(WaitForSingleObject(read_finish,5000)==WAIT_OBJECT_0);}
    return n;
}
static int truncate_driver(vnode_t *vn,u64 n){vn->size=n;return 0;}
static vnode_ops_t ops={.read=read_driver,.write=write_driver,.truncate=truncate_driver,
                      .sync=sync_driver,.release=release_driver};
static vnode_t fresh(void){return (vnode_t){.type=VN_FILE,.refs=1,.fs=&memory_fs,.ops=&ops};}
static file_t *opened(vnode_t *vn){file_t *f=NULL;assert(!vfs_open_vnode(vn,O_RDWR,&f));return f;}
static proc_t parent,child,sibling;
static DWORD WINAPI writer(void *unused){
    (void)unused;
    for(unsigned i=0;i<1000;i++){
        file_t *f=proc_fd_acquire(&sibling,0);assert(f);
        assert(vfs_write(f,"a",1)==1);vfs_close(f);
        assert(proc_fd_dup(&sibling,0,1)==1);
        f=proc_fd_acquire(&sibling,1);if(f){assert(f==parent.fds[0]);vfs_close(f);}
        int r=proc_fd_close(&sibling,1);assert(!r||r==-E_BADF);
    }
    return 0;
}
static DWORD WINAPI reader(void *arg){assert(vfs_read(arg,NULL,1)==1);return 0;}
static void clean(void){
    for(unsigned i=0;i<MAX_FILES;i++)assert(!files[i].in_use&&!files[i].refs);
    assert(!fs_busy&&!file_pool_lock.held&&!descriptor_lock.held&&irq_enabled);
}
int main(void){
    parent.pml4=child.pml4=sibling.pml4=0x1000;sibling.leader=&parent;
    vnode_t a=fresh(),b=fresh();file_t *f=opened(&a);
    assert(proc_fd_alloc(&parent,f)==0);
    file_t *held=proc_fd_acquire(&sibling,0);assert(held==f&&f->refs==2);
    assert(!proc_fd_close(&sibling,0)&&!parent.fds[0]&&held->in_use&&!releases);
    file_t *replacement=opened(&b);assert(replacement!=held);
    assert(proc_fd_alloc(&sibling,replacement)==0);
    vfs_close(held);assert(releases==1&&syncs==1&&parent.fds[0]==replacement);
    assert(!proc_fd_close(&parent,0)&&releases==2);clean();

    a=fresh();f=opened(&a);assert(proc_fd_alloc(&parent,f)==0);
    assert(proc_fd_dup(&sibling,0,-1)==1&&proc_fd_dup(&sibling,0,2)==2);
    assert(f->refs==3&&proc_fd_dup(&sibling,1,1)==1&&f->refs==3);
    assert(proc_fd_inherit_stdio(&child,&sibling)&&f->refs==6);
    assert(vfs_write(child.fds[1],"abc",3)==3&&parent.fds[2]->pos==3);
    proc_fd_close_all(&sibling);assert(f->refs==6);
    proc_fd_close_all(&parent);assert(f->refs==3);
    proc_fd_close_all(&child);clean();

    a=fresh();callback_vnode=fresh();retiring=opened(&a);
    vfs_close(retiring);assert(callback_file&&callback_file->in_use);
    vfs_close(callback_file);clean();

    a=fresh();b=fresh();f=opened(&a);replacement=opened(&b);int pair[2]={-1,-1};
    copy_fails=true;assert(proc_fd_install_pipe(&sibling,f,replacement,(u64)(uintptr_t)pair)==-E_INVAL);
    assert(!parent.fds[0]&&!parent.fds[1]&&f->refs==1&&replacement->refs==1);
    copy_fails=false;assert(!proc_fd_install_pipe(&sibling,f,replacement,(u64)(uintptr_t)pair));
    assert(pair[0]==0&&pair[1]==1&&parent.fds[0]==f&&parent.fds[1]==replacement);
    for(int i=2;i<PROC_MAX_FDS-1;i++)assert(proc_fd_dup(&parent,0,i)==i);
    vnode_t c=fresh(),d=fresh();file_t *cf=opened(&c),*df=opened(&d);
    pair[0]=pair[1]=-1;
    assert(proc_fd_install_pipe(&parent,cf,df,(u64)(uintptr_t)pair)==-E_MFILE);
    assert(pair[0]==-1&&!parent.fds[PROC_MAX_FDS-1]&&cf->refs==1&&df->refs==1);
    vfs_close(cf);vfs_close(df);proc_fd_close_all(&parent);clean();

    a=fresh();f=opened(&a);assert(proc_fd_alloc(&parent,f)==0);HANDLE threads[8];
    for(unsigned i=0;i<8;i++){threads[i]=CreateThread(NULL,0,writer,NULL,0,NULL);assert(threads[i]);}
    assert(WaitForMultipleObjects(8,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<8;i++)CloseHandle(threads[i]);
    assert(f->pos==8000&&a.size==8000&&f->refs==1);
    assert(vfs_seek(f,-8000,1)==0);assert(vfs_seek(f,INT64_MIN,1)==-E_INVAL);
    assert(vfs_seek(f,INT64_MAX,0)==INT64_MAX&&vfs_seek(f,1,1)==-E_INVAL);
    a.size=UINT64_MAX;assert(vfs_seek(f,1,2)==-E_INVAL);
    assert(vfs_truncate(f,0)==0);proc_fd_close_all(&parent);clean();

    a=fresh();a.type=VN_CHR;f=opened(&a);
    read_started=CreateEvent(NULL,TRUE,FALSE,NULL);read_finish=CreateEvent(NULL,TRUE,FALSE,NULL);
    HANDLE t=CreateThread(NULL,0,reader,f,0,NULL);assert(t);
    assert(WaitForSingleObject(read_started,5000)==WAIT_OBJECT_0);
    assert(vfs_write(f,"x",1)==1&&!f->pos&&!f->io_busy); // read is still blocked
    SetEvent(read_finish);assert(WaitForSingleObject(t,5000)==WAIT_OBJECT_0);
    CloseHandle(t);CloseHandle(read_started);CloseHandle(read_finish);vfs_close(f);clean();
    puts("PASS production FD/VFS: retained close/reuse, shared dup offsets, sibling stdio ownership, retiring pool exclusion, atomic pipe publication, 8 host threads/8000 writes plus dup/close races, seek overflow, console duplex; NOT native AP validation");
}
'''

if __name__ == '__main__':
    run_test(code, 'process-fds')
