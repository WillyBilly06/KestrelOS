#!/usr/bin/env python3
"""Production device-control staging/dispatch; VM and hardware edges mocked.

Real host threads cover a blocked ioctl concurrent with unmap, descriptor close,
and descriptor-number reuse. Does not execute native APs or audio hardware.
"""
from test_vfs_user_io import code as io_code
from test_gpu_stable_candidate import ROOT, function, run_test

code = io_code.split('static proc_t process=')[0]
code += r'''
#define VFS_NAME_MAX 255
enum {CON_GET_SIZE=1,CON_SET_CURSOR=2,CON_GET_CURSOR=3,CON_SET_RAW=4,CON_CLEAR=5,CON_SHOW_CURSOR=6};
static const char usb_audio_marker;
static u32 endpoints[24]; // only array capacity is used by the actual audio schema
'''
header = (ROOT / 'kernel/vfs.h').read_text()
start = header.index('typedef struct devfs_ops {')
code += header[start:header.index('} devfs_ops_t;', start)+len('} devfs_ops_t;')]
dev = (ROOT / 'kernel/devfs.c').read_text()
start = dev.index('typedef struct {')
code += dev[start:dev.index('} devfs_entry_t;', start)+len('} devfs_entry_t;')]
for src, name in (('tty.c','tty_ioctl_shape'),('eventq.c','input_dev_ioctl_shape'),
                  ('hda.c','audio_dev_ioctl_shape'),('pipe.c','pipe_ioctl_shape'),
                  ('devfs.c','dev_ioctl_shape'),('devfs.c','dev_ioctl'),('vfs.c','vfs_ioctl')):
    code += '\n' + function((ROOT / ('kernel/'+src)).read_text(), name) + '\n'
code += r'''
static proc_t process={.pml4=0x1000};
enum {SYS_IOCTL=1,SYS_CONSOLE=2,STDOUT_FD=1};
'''
native = (ROOT / 'kernel/syscall.c').read_text()
code += 'static s64 native_ioctl(proc_t*p,u64 nr,u64 a0,u64 a1,u64 a2){switch(nr){\n'
for a, b in (('    case SYS_IOCTL: {','    case SYS_SPAWN:'),
             ('    case SYS_CONSOLE: {','    case SYS_POWEROFF:')):
    code += native[native.index(a):native.index(b,native.index(a))]
code += 'default:return -E_INVAL;}}\n'
code += r'''
static unsigned char user[512];
static unsigned calls;
static bool do_block,expect_null;
static int driver_result;
static HANDLE entered,finish;
static vfs_ioctl_shape_t expected;
static int driver(void*ctx,u32 cmd,void*arg){
    (void)ctx;(void)cmd;calls++;
    assert(irq_enabled&&!vm_owned);
    if(expect_null){assert(!arg);return driver_result;}
    size_t cap=expected.in_bytes>expected.out_bytes?expected.in_bytes:expected.out_bytes;
    assert(arg&&!mapped((u64)(uintptr_t)arg,cap,false));
    unsigned char*b=arg;
    for(unsigned i=0;i<expected.in_bytes;i++)assert(b[i]==0x5a);
    for(size_t i=expected.in_bytes;i<cap;i++)assert(b[i]==0); // zeroed unused slots
    if(do_block){SetEvent(entered);assert(WaitForSingleObject(finish,5000)==WAIT_OBJECT_0);
        for(unsigned i=0;i<expected.in_bytes;i++)assert(b[i]==0x5a);}
    // Deliberately leave output holes, as format/endpoint enumeration does.
    if(expected.out_bytes){b[0]=0x12;b[expected.out_bytes-1]=0x34;}
    return driver_result;
}
static devfs_ops_t devops={.ioctl=driver};
static devfs_entry_t entry={.ops=&devops,.used=true};
static vnode_ops_t ops={.ioctl=dev_ioctl,.ioctl_shape=dev_ioctl_shape};
static filesystem_t memory_fs={true};
static vnode_t vn;
static file_t*f;
static void reset(int(*shape)(void*,u32,vfs_ioctl_shape_t*),void*ctx,u32 cmd,size_t bytes){
    proc_fd_close_all(&process);assert(!allocations);
    calls=region_count=0;attempts=0;fail_on=0;expect_null=do_block=false;driver_result=0;
    devops.ioctl_shape=shape;entry.ctx=ctx;entry.used=true;process.is_kernel=false;
    vn=(vnode_t){.type=VN_CHR,.refs=1,.fs=&memory_fs,.ops=&ops,.priv=&entry};
    assert(!vfs_open_vnode(&vn,O_RDWR,&f));assert(proc_fd_alloc(&process,f)==0);
    assert(!shape(ctx,cmd,&expected));memset(user,0x5a,sizeof user);map(user,bytes);
}
static void shape_is(int(*shape)(void*,u32,vfs_ioctl_shape_t*),void*ctx,u32 cmd,u32 in,u32 out,bool required){
    vfs_ioctl_shape_t s={0};assert(!shape(ctx,cmd,&s));
    assert(s.in_bytes==in&&s.out_bytes==out&&s.required==required);
}
static DWORD WINAPI blocked(void*unused){
    (void)unused;assert(native_ioctl(&process,SYS_IOCTL,0,3,(u64)(uintptr_t)user)==-E_INVAL);return 0;
}
int main(void){
    endpoints[0]=0; // retain fixture storage even though schemas only use sizeof
    shape_is(tty_ioctl_shape,NULL,1,0,8,true);shape_is(tty_ioctl_shape,NULL,2,8,0,true);
    shape_is(tty_ioctl_shape,NULL,3,0,8,true);shape_is(tty_ioctl_shape,NULL,4,4,0,true);
    shape_is(tty_ioctl_shape,NULL,5,0,0,false);shape_is(tty_ioctl_shape,NULL,6,4,0,true);
    shape_is(input_dev_ioctl_shape,NULL,1,0,4,false);shape_is(input_dev_ioctl_shape,NULL,2,0,0,false);
    shape_is(input_dev_ioctl_shape,NULL,3,4,4,false);
    shape_is(audio_dev_ioctl_shape,NULL,1,0,4,false);shape_is(audio_dev_ioctl_shape,NULL,2,0,0,false);
    shape_is(audio_dev_ioctl_shape,NULL,3,0,12,false);shape_is(audio_dev_ioctl_shape,(void*)&usb_audio_marker,3,0,12,true);
    shape_is(audio_dev_ioctl_shape,NULL,4,0,4,false);shape_is(audio_dev_ioctl_shape,NULL,5,0,4,false);
    shape_is(audio_dev_ioctl_shape,NULL,8,0,76,true);shape_is(audio_dev_ioctl_shape,NULL,9,8,0,true);
    shape_is(audio_dev_ioctl_shape,NULL,10,0,392,true);
    for(unsigned c=11;c<=13;c++)shape_is(audio_dev_ioctl_shape,NULL,c,4,0,true);
    vfs_ioctl_shape_t s;assert(!pipe_ioctl_shape(NULL,1,&s)&&!s.in_bytes&&s.out_bytes==4&&!s.required);
    assert(!pipe_ioctl_shape(NULL,2,&s)&&!s.in_bytes&&!s.out_bytes&&!s.required);
    assert(pipe_ioctl_shape(NULL,999,&s)==-E_INVAL);
    assert(tty_ioctl_shape(NULL,999,&s)==-E_INVAL&&input_dev_ioctl_shape(NULL,999,&s)==-E_INVAL);
    assert(audio_dev_ioctl_shape(NULL,999,&s)==-E_INVAL);

    reset(audio_dev_ioctl_shape,NULL,10,392);
    assert(!native_ioctl(&process,SYS_IOCTL,0,10,(u64)(uintptr_t)user)&&calls==1&&f->refs==1&&!allocations);
    assert(user[0]==0x12&&user[391]==0x34&&user[392]==0x5a);
    for(unsigned i=1;i<391;i++)assert(!user[i]);
    reset(audio_dev_ioctl_shape,NULL,10,391);
    assert(native_ioctl(&process,SYS_IOCTL,0,10,(u64)(uintptr_t)user)==-E_INVAL&&!calls&&!allocations);
    reset(audio_dev_ioctl_shape,NULL,3,8); // old eight-byte check would let a twelve-byte write through
    assert(native_ioctl(&process,SYS_IOCTL,0,3,(u64)(uintptr_t)user)==-E_INVAL&&!calls);
    reset(audio_dev_ioctl_shape,NULL,9,8);regions[0].writable=false;
    assert(!native_ioctl(&process,SYS_IOCTL,0,9,(u64)(uintptr_t)user)&&calls==1); // input needn't be writable
    reset(tty_ioctl_shape,NULL,1,8);regions[0].writable=false;
    assert(native_ioctl(&process,SYS_IOCTL,0,1,(u64)(uintptr_t)user)==-E_INVAL&&!calls);
    assert(native_ioctl(&process,SYS_IOCTL,0,1,0)==-E_INVAL&&!calls); // required NULL must not enter TTY
    reset(tty_ioctl_shape,NULL,5,0);expect_null=true;
    assert(!native_ioctl(&process,SYS_IOCTL,0,5,UINT64_MAX)&&calls==1); // no payload
    reset(input_dev_ioctl_shape,NULL,3,0);expect_null=true;
    assert(!native_ioctl(&process,SYS_IOCTL,0,3,0)&&calls==1); // optional stays NULL
    reset(audio_dev_ioctl_shape,(void*)&usb_audio_marker,3,0);
    assert(native_ioctl(&process,SYS_IOCTL,0,3,0)==-E_INVAL&&!calls);
    reset(audio_dev_ioctl_shape,NULL,8,76);fail_on=1;
    assert(native_ioctl(&process,SYS_IOCTL,0,8,(u64)(uintptr_t)user)==-E_NOMEM&&!calls&&f->refs==1&&!allocations);
    fail_on=0;driver_result=-E_IO;
    assert(native_ioctl(&process,SYS_IOCTL,0,8,(u64)(uintptr_t)user)==-E_IO&&user[0]==0x5a);
    assert(native_ioctl(&process,SYS_IOCTL,0,999,(u64)(uintptr_t)user)==-E_INVAL&&calls==1);
    devops.ioctl_shape=NULL;
    assert(native_ioctl(&process,SYS_IOCTL,0,8,(u64)(uintptr_t)user)==-E_NOSYS&&calls==1);
    reset(tty_ioctl_shape,NULL,1,8);assert(proc_fd_dup(&process,0,STDOUT_FD)==STDOUT_FD);
    assert(!native_ioctl(&process,SYS_CONSOLE,1,(u64)(uintptr_t)user,0)&&calls==1&&f->refs==2);

    reset(input_dev_ioctl_shape,NULL,3,4);do_block=true;
    entered=CreateEvent(NULL,TRUE,FALSE,NULL);finish=CreateEvent(NULL,TRUE,FALSE,NULL);assert(entered&&finish);
    HANDLE thread=CreateThread(NULL,0,blocked,NULL,0,NULL);assert(thread);
    assert(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    // Driver has private input, and holds the old description even after close/reuse.
    AcquireSRWLockExclusive(&vm_lock);regions[0].valid=false;memset(user,0xee,sizeof user);ReleaseSRWLockExclusive(&vm_lock);
    assert(!proc_fd_close(&process,0)&&f->in_use&&f->refs==1);
    vnode_t other={.type=VN_CHR,.refs=1,.fs=&memory_fs,.ops=&ops,.priv=&entry};file_t*replacement;
    assert(!vfs_open_vnode(&other,O_RDWR,&replacement)&&replacement!=f);
    assert(proc_fd_alloc(&process,replacement)==0);
    SetEvent(finish);assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    assert(!f->refs&&!f->in_use&&replacement->refs==1&&process.fds[0]==replacement&&user[0]==0xee&&!allocations);
    CloseHandle(thread);CloseHandle(entered);CloseHandle(finish);proc_fd_close_all(&process);
    puts("PASS production ioctl staging + native console/device dispatch + real schemas: exact sizes, NULL policy, permissions, output zeroing, error/OOM cleanup, blocked callback with concurrent unmap/close/FD reuse; VM/hardware mocked, not native SMP");
}
'''
if __name__ == '__main__':
    run_test(code, 'vfs-user-ioctl')
