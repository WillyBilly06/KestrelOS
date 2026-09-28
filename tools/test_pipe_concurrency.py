#!/usr/bin/env python3
"""Execute production pipe + VFS code, mocking scheduler handoff on host threads.

No native AP execution is emulated. Scheduler event-lock behavior is independently
covered by test_input_event_wait.py. Allocations and file-pool exhaustion are
injected here to verify pipe construction cleanup.
"""
from test_process_fds import code as fd_code
from test_gpu_stable_candidate import ROOT, run_test

code = fd_code.split('static filesystem_t memory_fs=')[0]
code += r'''
enum {E_INTR=4,E_PERM=1,E_NOMEM=12};
typedef uint8_t u8;
static volatile LONG allocations,allocation_attempt;
static LONG fail_allocation;
static void *kmalloc(size_t n){
    LONG attempt=InterlockedIncrement(&allocation_attempt);
    if(attempt==fail_allocation)return NULL;
    void *p=malloc(n);if(p)InterlockedIncrement(&allocations);return p;
}
static void *kzalloc(size_t n){void *p=kmalloc(n);if(p)memset(p,0,n);return p;}
static void kfree(void *p){if(p){free(p);InterlockedDecrement(&allocations);}}
static _Thread_local bool stop;
static bool proc_stop_requested(void){return stop;}
static void sched_sleep_ms(u64 n){Sleep((DWORD)n);}
static SRWLOCK event_lock=SRWLOCK_INIT;
static CONDITION_VARIABLE event_changed=CONDITION_VARIABLE_INIT;
static volatile LONG waits;
static bool cancel_on_wait;
static void (*before_wait)(void);
static int sched_wait_event(u64 *event,u64 expected,u64 deadline){
    assert(irq_enabled&&deadline==~0ull);InterlockedIncrement(&waits);
    if(cancel_on_wait){stop=true;return -1;}
    if(before_wait){void (*hook)(void)=before_wait;before_wait=NULL;hook();}
    AcquireSRWLockExclusive(&event_lock);
    while(__atomic_load_n(event,__ATOMIC_ACQUIRE)==expected){
        if(!SleepConditionVariableSRW(&event_changed,&event_lock,5000,0)){
            fprintf(stderr,"lost pipe wakeup\n");abort();
        }
    }
    ReleaseSRWLockExclusive(&event_lock);return 1;
}
static void sched_signal_event(u64 *event){
    assert(irq_enabled);AcquireSRWLockExclusive(&event_lock);
    __atomic_fetch_add(event,1ull,__ATOMIC_RELEASE);
    WakeAllConditionVariable(&event_changed);ReleaseSRWLockExclusive(&event_lock);
}
'''
src = (ROOT / 'kernel/pipe.c').read_text()
code += src[src.index('#define PIPE_CAPACITY'):]
code += r'''
static file_t *rf,*wf;
static void inject_write(void){assert(vfs_write(wf,"W",1)==1);}
static void inject_close(void){vfs_close(wf);wf=NULL;}
static DWORD WINAPI producer(void *arg){
    file_t *f=arg;char data[173];memset(data,'P',sizeof data);
    for(unsigned i=0;i<300;i++)assert(vfs_write(f,data,sizeof data)==sizeof data);
    vfs_close(f);return 0;
}
static DWORD WINAPI consumer(void *arg){
    file_t *f=arg;char data[227];u64 bytes=0;
    for(;;){ssize_t_k n=vfs_read(f,data,sizeof data);assert(n>=0);if(!n)break;
        for(ssize_t_k i=0;i<n;i++)assert(data[i]=='P');bytes+=n;}
    assert(bytes==4u*300u*173u);vfs_close(f);return 0;
}
static vnode_ops_t dummy_ops;
static void clean(void){
    assert(!allocations);
    for(unsigned i=0;i<MAX_FILES;i++)assert(!files[i].in_use&&!files[i].refs);
    assert(!fs_busy&&irq_enabled);
}
int main(void){
    for(LONG fail=1;fail<=6;fail++){
        allocation_attempt=0;fail_allocation=fail;rf=wf=(file_t*)1;
        assert(pipe_create(&rf,&wf)==-E_NOMEM&&!rf&&!wf);clean();
    }
    fail_allocation=0;
    for(unsigned available=0;available<=1;available++){
        vnode_t vn={.type=VN_CHR,.ops=&dummy_ops,.refs=1};file_t *held[MAX_FILES];
        for(unsigned i=0;i<MAX_FILES-available;i++){
            assert(!vfs_open_vnode(i?vnode_ref(&vn):&vn,O_RDWR,&held[i]));
        }
        assert(pipe_create(&rf,&wf)==-E_MFILE&&!rf&&!wf&&!allocations);
        for(unsigned i=0;i<MAX_FILES-available;i++)vfs_close(held[i]);clean();
    }
    assert(!pipe_create(&rf,&wf));char byte;
    before_wait=inject_write;assert(vfs_read(rf,&byte,1)==1&&byte=='W');
    before_wait=inject_close;assert(vfs_read(rf,&byte,1)==0&&!wf);vfs_close(rf);clean();

    assert(!pipe_create(&rf,&wf));cancel_on_wait=true;
    assert(vfs_read(rf,&byte,1)==-E_INTR&&stop);stop=false;
    char bulk[PIPE_CAPACITY];memset(bulk,'P',sizeof bulk);
    assert(vfs_write(wf,bulk,PIPE_CAPACITY-2)==PIPE_CAPACITY-2);
    assert(vfs_write(wf,"abc",3)==2&&stop);stop=false;cancel_on_wait=false;
    vfs_close(rf);assert(vfs_write(wf,"q",1)==-E_PERM);vfs_close(wf);clean();

    assert(!pipe_create(&rf,&wf));HANDLE tasks[5];
    for(unsigned i=0;i<4;i++){
        file_t *f=vfs_file_ref(wf);assert(f);tasks[i]=CreateThread(NULL,0,producer,f,0,NULL);assert(tasks[i]);
    }
    vfs_close(wf);wf=NULL;
    tasks[4]=CreateThread(NULL,0,consumer,rf,0,NULL);assert(tasks[4]);
    assert(WaitForMultipleObjects(5,tasks,TRUE,15000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<5;i++)CloseHandle(tasks[i]);clean();
    puts("PASS production pipe/VFS: six allocation failures, both file-pool failures, write/close before sleep, cancellation and partial write, four producers + consumer/207600 bytes, final-writer EOF, no leaked objects; scheduler host adapter, NOT native AP validation");
}
'''
if __name__ == '__main__':
    run_test(code, 'pipe-concurrency')
