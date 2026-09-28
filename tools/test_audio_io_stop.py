#!/usr/bin/env python3
"""Production audio read/write stop/error boundaries; device ring mocked."""
from test_gpu_stable_candidate import ROOT, run_test

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
typedef uint8_t u8;typedef uint64_t u64;typedef int64_t ssize_t_k;
enum {E_INTR=4,E_IO=5,E_NODEV=19,E_INVAL=22};
static const char usb_audio_marker;
static bool ready,capture_ready,usb_ready,stopped;
static unsigned calls,sleeps;
static int results[4],last_request;
static bool stop_on_sleep,stop_on_call,disconnect_on_sleep;
static bool proc_stop_requested(void){return stopped;}
static bool usbaudio_can_play(void){return usb_ready;}
static void sched_sleep_ms(u64 ms){
    assert(ms==5);sleeps++;
    if(stop_on_sleep)stopped=true;
    if(disconnect_on_sleep)ready=capture_ready=usb_ready=false;
    assert(sleeps<=400); // catches old infinite/busy wait
}
static int transfer(int bytes){
    assert(bytes>0);last_request=bytes;
    if(stop_on_call)stopped=true;
    int n=results[calls<4?calls:3];calls++;return n;
}
static int hda_write(const void*p,int n){(void)p;assert(ready);return transfer(n);}
static int usbaudio_write(const void*p,int n){(void)p;assert(usb_ready);return transfer(n);}
static int hda_read(void*p,int n){(void)p;assert(capture_ready);return transfer(n);}
'''
src = (ROOT / 'kernel/hda.c').read_text()
for name in ('audio_dev_write','audio_dev_read'):
    start = src.index('static ssize_t_k '+name+'(')
    code += src[start:src.index('\n}',start)+2]+'\n'
code += r'''
static void reset(void){
    ready=capture_ready=usb_ready=true;stopped=false;calls=sleeps=0;
    stop_on_sleep=stop_on_call=disconnect_on_sleep=false;
    for(unsigned i=0;i<4;i++)results[i]=0;
}
int main(void){
    char b[32];reset();stopped=true;
    assert(audio_dev_write(NULL,b,32,0)==-E_INTR&&!calls&&!sleeps);
    assert(audio_dev_read(NULL,b,32,0)==-E_INTR&&!calls&&!sleeps);
    assert(!audio_dev_write(NULL,b,0,0)&&!audio_dev_read(NULL,b,0,0));
    reset();stop_on_sleep=true;
    assert(audio_dev_write(NULL,b,32,0)==-E_INTR&&calls==1&&sleeps==1);
    reset();stop_on_sleep=true;
    assert(audio_dev_read(NULL,b,32,0)==-E_INTR&&calls==1&&sleeps==1);
    reset();stop_on_call=true;results[0]=7;
    assert(audio_dev_write(NULL,b,32,0)==7&&calls==1&&!sleeps);
    reset();results[0]=-E_IO;
    assert(audio_dev_write(NULL,b,32,0)==-E_IO&&calls==1&&!sleeps);
    reset();results[0]=7;results[1]=-E_IO;
    assert(audio_dev_write(NULL,b,32,0)==7&&calls==2&&!sleeps);
    reset();results[0]=-E_IO;
    assert(audio_dev_read(NULL,b,32,0)==-E_IO&&calls==1&&!sleeps);
    reset();disconnect_on_sleep=true;
    assert(audio_dev_write((void*)&usb_audio_marker,b,32,0)==-E_NODEV&&calls==1&&sleeps==1);
    reset();disconnect_on_sleep=true;
    assert(audio_dev_read(NULL,b,32,0)==-E_NODEV&&calls==1&&sleeps==1);
    reset();results[0]=33;
    assert(audio_dev_write(NULL,b,32,0)==-E_IO);
    reset();results[0]=33;
    assert(audio_dev_read(NULL,b,32,0)==-E_IO);
    reset();stop_on_call=true;results[0]=7;
    assert(audio_dev_write(NULL,b,(size_t)1<<32,0)==7&&last_request==0x7fffffff);
    reset();results[0]=1;
    assert(audio_dev_read(NULL,b,(size_t)1<<32,0)==1&&last_request==0x7fffffff);
    reset();results[0]=7;results[1]=25;
    assert(audio_dev_write(NULL,b,32,0)==32&&calls==2&&!sleeps);
    reset();assert(!audio_dev_read(NULL,b,32,0)&&calls==400&&sleeps==400);
    puts("PASS production audio stop/error/partial/disconnect boundaries and bounded integer requests; scheduler/device ring mocked, no native audio test");
}
'''
if __name__ == '__main__':
    run_test(code, 'audio-io-stop')
