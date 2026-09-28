#!/usr/bin/env python3
"""Production TTY cancellation. Pipe cancellation is in test_pipe_concurrency.py."""
from test_gpu_stable_candidate import ROOT, function, run_test

tty = (ROOT / 'kernel/tty.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;typedef long ssize_t_k;
enum{PROC_MAX=8,PROC_BLOCKED=1,E_INTR=4,E_BADF=9,E_PERM=1};
typedef struct{int id;}proc_t;
static proc_t process;
static bool stopped,irq_on=true;
static unsigned blocks,wakes;
static proc_t *proc_current(void){return &process;}
static bool proc_stop_requested(void){return stopped;}
static bool irq_save(void){bool old=irq_on;irq_on=false;return old;}
static void irq_restore(bool old){irq_on=old;}
static void sched_sleep_ms(u64 ms){(void)ms;stopped=true;}
static void sched_block(int why){assert(why==PROC_BLOCKED&&irq_on);blocks++;stopped=true;}
static proc_t *waiters[PROC_MAX];
static char line[512];
static int line_ready,line_taken;
static bool at_eof;
static void pump(void){}
typedef struct{void*priv;}vnode_t;
'''
for name in ('tty_wait', 'tty_read'):
    if name == 'tty_read':
        start = tty.index('static ssize_t_k tty_read(')
        code += tty[start:tty.index('\n}', start)+2]+'\n'
    else:
        code += function(tty, name)+'\n'
code += r'''
static void reset(void){stopped=false;irq_on=true;blocks=wakes=0;memset(waiters,0,sizeof waiters);}
static void empty(proc_t **w){for(unsigned i=0;i<8;i++)assert(!w[i]);}
int main(void){
    char output[16];reset();assert(tty_read(NULL,output,sizeof output,0)==-E_INTR);
    assert(blocks==1&&irq_on);empty(waiters);
    reset();stopped=true;assert(tty_read(NULL,output,sizeof output,0)==-E_INTR&&!blocks);
    puts("PASS production TTY stop: cancellation before wait and after wake, waiter deregistration, IRQ restoration; scheduler/driver storage mocked");
}
'''
if __name__ == '__main__':
    run_test(code, 'blocking-io-stop')
