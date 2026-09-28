#!/usr/bin/env python3
"""Run the real video utility against modeled file/syscall boundaries."""
from pathlib import Path
from test_gpu_stable_candidate import run_test
ROOT=Path(__file__).resolve().parents[1]


def main():
    c=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define O_RDONLY 0
#define off_t int64_t
#define ssize_t intptr_t
static unsigned mode,allocs,frees,alloc_calls,fail_alloc,opened,closed,position,inspects,decodes,writes,sleeps,busy_left;
static int fake_open(const char *p,int f){assert(!strcmp(p,"input.h264")&&f==0);if(mode==8)return -1;opened++;return 7;}
static int fake_close(int fd){assert(fd==7);closed++;return 0;}
static int64_t fake_lseek(int fd,int64_t off,int whence){assert(fd==7&&!off);return whence==SEEK_END?(mode==9?0:16):0;}
static intptr_t fake_read(int fd,void *out,size_t n){
    assert(fd==7);if(mode==10 && position>=3)return 0;
    if(n>3)n=3;memset(out,0x5a,n);position+=(unsigned)n;return (intptr_t)n;
}
static void *fake_malloc(size_t n){if(++alloc_calls==fail_alloc)return NULL;allocs++;return malloc(n);}
static void fake_free(void *p){if(p){frees++;free(p);}}
static void sleep_ms(uint64_t ms){assert(ms==10);sleeps++;}
static int write_file(const char *p,const void *data,size_t n){
    assert(!strcmp(p,"output.nv12")&&n==6144&&decodes==1);
    for(unsigned i=0;i<n;i++)assert(((const unsigned char*)data)[i]==0x39);
    writes++;return mode==5?-1:0;
}
'''
    c+='#include "'+(ROOT/'include/kestrel/video.h').as_posix()+'"\n'
    c+=r'''
static int gpu_video(kvideo_request_t *r){
    assert(r->version==1&&r->input_bytes==16);
    for(unsigned i=0;i<16;i++)assert(((unsigned char*)(uintptr_t)r->input)[i]==0x5a);
    if(mode==7||busy_left){if(busy_left)busy_left--;errno=EBUSY;return -1;}
    if(r->operation==0){
        inspects++;r->coded_width=r->coded_height=r->display_width=r->display_height=r->pitch=64;
        r->required_bytes=6144;r->written_bytes=0;r->phase=KVIDEO_PHASE_COMPLETE;
        if(mode==1){errno=EINVAL;r->phase=KVIDEO_PHASE_HEADERS;r->parse_status=3;return -1;}
        return 0;
    }
    assert(r->operation==1&&r->output_capacity==6144&&r->output);decodes++;
    memset((void*)(uintptr_t)r->output,0x39,6144);r->written_bytes=6144;r->decoded_mbs=16;
    if(mode==2){errno=EIO;r->phase=KVIDEO_PHASE_STATUS;return -1;}
    if(mode==3)r->phase=KVIDEO_PHASE_RELEASE;
    if(mode==4)r->written_bytes=6143;
    return 0;
}
#define malloc fake_malloc
#define free fake_free
#define open fake_open
#define close fake_close
#define read fake_read
#define lseek fake_lseek
'''
    src=(ROOT/'user/video/video.c').read_text().replace('#include "kestrel.h"','')
    c+=src.replace('int main(int argc,char **argv)','int video_main(int argc,char **argv)')
    c+=r'''
static void setup(unsigned m){
    assert(allocs==frees&&opened==closed);
    mode=m;allocs=frees=alloc_calls=fail_alloc=opened=closed=position=inspects=decodes=writes=sleeps=busy_left=0;
}
int main(void){
    char *decode[]={"video","decode","input.h264","output.nv12"};
    char *info[]={"video","info","input.h264"};
    setup(0);assert(!video_main(3,info)&&inspects==1&&!decodes&&!writes);
    setup(0);assert(!video_main(4,decode)&&inspects==1&&decodes==1&&writes==1);
    for(unsigned m=1;m<=10;m++){
        setup(m);if(m==6)busy_left=2;
        assert(video_main(4,decode)==(m==6?0:1));
        if(m==6)assert(writes==1&&sleeps==2);
        else if(m==5)assert(writes==1);
        else assert(!writes);
        if(m==7)assert(sleeps==100&&!decodes&&!inspects);
    }
    for(unsigned a=1;a<=2;a++){setup(0);fail_alloc=a;assert(video_main(4,decode)==1&&!writes);}
    setup(0);assert(video_main(1,decode)==1&&!opened);
    setup(0);decode[3]=decode[2];assert(video_main(4,decode)==1&&!opened);
    assert(allocs==frees&&opened==closed);
    puts("PASS actual video app: info/decode, short reads, no predecode output writes, error/incomplete output gates, bounded busy retry, allocation/file failure cleanup (modeled file and GPU APIs)");
    return 0;
}
'''
    run_test(c,'video_app')


if __name__=='__main__':main()
