#!/usr/bin/env python3
"""Test exact NV906F packet recognition without touching GPU/process memory."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    header=(ROOT/'tools/nvenc_method_capture.h').as_posix()
    code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
'''+f'#include "{header}"\n'+r'''
static uint32_t words[1100];static unsigned n;
static void packet(unsigned op,unsigned sub,unsigned method,unsigned value){
    words[n++]=(op<<29)|(sub<<13)|(method/4)|((op==4?value:1)<<16);
    if(op!=4)words[n++]=value;
}
static void sequence(unsigned op,unsigned sub){
    n=0;packet(op,sub,0x200,1);packet(op,sub,0x700,0x1303);
    packet(op,sub,0x704,0);
    const unsigned addresses[]={0x710,0x718,0x71c,0x734,0x740,0x744};
    for(unsigned i=0;i<6;i++)packet(op,sub,addresses[i],0x100+i);
    packet(op,sub,0x300,0);
}
int main(void){
    const unsigned ops[]={1,3,4,5};nvenc_capture_sequence result,guard;
    memset(&guard,0xa5,sizeof guard);
    for(unsigned op=0;op<4;op++)for(unsigned sub=0;sub<8;sub++){
        sequence(ops[op],sub);assert(nvenc_capture_decode(words,n,&result));
        assert(result.count==10&&result.words==n&&result.subchannel==sub);
        assert(result.application_id_observed);
        unsigned skip=ops[op]==4?1:2;
        assert(nvenc_capture_decode(words+skip,n-skip,&result));
        assert(!result.application_id_observed&&result.count==9);
        for(unsigned i=0;i<n;i++){
            result=guard;assert(!nvenc_capture_decode(words,i,&result));
            assert(!memcmp(&result,&guard,sizeof result));
        }
        /* Application-id alone, decode codec, missing/other-channel input,
         * premature execute and malformed method headers are not evidence. */
        for(unsigned i=0;i<10;i++){
            sequence(ops[op],sub);unsigned at=i*(ops[op]==4?1:2);
            words[at]^=0x2000;assert(!nvenc_capture_decode(words,n,&result));
        }
        sequence(ops[op],sub);words[0]|=0x1000;
        assert(!nvenc_capture_decode(words,n,&result));
    }
    sequence(1,4);words[3]=4;assert(!nvenc_capture_decode(words,n,&result));
    sequence(1,4);words[n-2]=0x20000000|0x8000|(0x300/4)|(8191u<<16);
    assert(!nvenc_capture_decode(words,n,&result));
    sequence(1,4);memmove(words+3,words+2,(n-2)*4);words[2]=0;n++;
    assert(nvenc_capture_decode(words,n,&result));
    /* Multiple incrementing values and one-increment packet semantics. */
    n=0;packet(4,2,0x200,1);words[n++]=(1u<<29)|(2u<<13)|(2u<<16)|(0x700/4);
    words[n++]=0x1303;words[n++]=0;
    packet(1,2,0x710,0x100);words[n++]=(1u<<29)|(2u<<13)|(2u<<16)|(0x718/4);
    words[n++]=0x200;words[n++]=0x300;
    packet(1,2,0x734,0x400);words[n++]=(5u<<29)|(2u<<13)|(3u<<16)|(0x740/4);
    words[n++]=0x500;words[n++]=0x600;words[n++]=0x600;
    packet(4,2,0x300,0);assert(nvenc_capture_decode(words,n,&result));
    assert(result.methods[result.count-2].method==0x744);
    /* Actual Blackwell address packets: zero upper words are mandatory, not
     * invalid NULL bindings. Also cover addresses whose low word is zero. */
    for(unsigned high_only=0;high_only<2;high_only++){
        n=0;packet(1,4,0x200,1);packet(1,4,0x700,0x1303);packet(1,4,0x704,0);
        const unsigned addresses[]={0x710,0x718,0x71c,0x734,0x740,0x744};
        for(unsigned i=0;i<6;i++){
            words[n++]=(3u<<29)|(2u<<16)|(4u<<13)|(addresses[i]/4);
            words[n++]=high_only?0:0x8000010+i;words[n++]=high_only?1:0;
        }
        packet(1,4,0x300,0);
        assert(nvenc_capture_decode(words,n,&result) && result.count==16);
        assert(result.methods[3].method==0x710 && result.methods[4].method==0x710);
        for(unsigned i=0;i<n;i++)assert(!nvenc_capture_decode(words,i,&result));
        for(unsigned i=0;i<6;i++){
            unsigned low=7+i*3,high=low+1;uint32_t lo=words[low],hi=words[high];
            words[low]=words[high]=0;assert(!nvenc_capture_decode(words,n,&result));
            words[low]=lo;words[high]=0x1000000;assert(!nvenc_capture_decode(words,n,&result));
            words[high]=hi;
        }
    }
    uint32_t rng=1;
    for(unsigned k=0;k<20000;k++){
        for(unsigned i=0;i<1100;i++){rng=rng*1664525+1013904223;words[i]=rng;}
        words[0]=0x80010080; /* plausible app1 prefix, all else random */
        result=guard;assert(!nvenc_capture_decode(words,k%1100,&result));
        assert(!memcmp(&result,&guard,sizeof result));
    }
    assert(!nvenc_capture_decode(NULL,4,&result));
    assert(!nvenc_capture_decode(words,4,NULL));
    puts("NVENC_METHOD_CAPTURE_PASS packet forms, subchannels, required launch state, truncation, overflow, 20000 malformed streams");
}
'''
    run_test(code,'nvenc_method_capture')
    # Execute the actual page-admission wrapper with modeled Windows queries.
    source=(ROOT/'tools/generate_nvenc_h264_fixture.cu').read_text()
    safe=function(source,'read_owned_data')
    safe=safe.replace('reinterpret_cast<void*>(at)','(void*)at').replace(
        'reinterpret_cast<uintptr_t>(r.BaseAddress)','(uintptr_t)r.BaseAddress').replace(
        'reinterpret_cast<void*>(address)','(void*)address')
    safety=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef void *HANDLE;typedef unsigned DWORD;
#define MEM_COMMIT 0x1000
#define PAGE_READWRITE 4
#define PAGE_WRITECOPY 8
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400
typedef struct {void *BaseAddress;size_t RegionSize;DWORD State,Protect;} MEMORY_BASIC_INFORMATION;
static unsigned prot[2],state[2],reads;
static bool fail_query,fail_read;
static size_t VirtualQuery(void *p,MEMORY_BASIC_INFORMATION *r,size_t n){
    uintptr_t a=(uintptr_t)p;assert(n==sizeof *r);
    if(fail_query||a<0x1000||a>=0x3000)return 0;
    unsigned i=(a-0x1000)/0x1000;
    *r=(MEMORY_BASIC_INFORMATION){(void*)(uintptr_t)(0x1000+i*0x1000),0x1000,state[i],prot[i]};
    return sizeof *r;
}
static bool ReadProcessMemory(HANDLE h,void *p,void *out,size_t n,size_t *got){
    assert(h==(HANDLE)1&&out&&p);reads++;*got=fail_read?n/2:n;return true;
}
'''+safe+r'''
int main(void){
    char data[64];size_t got;
    for(unsigned p=0;p<2048;p++){
        prot[0]=PAGE_READWRITE;prot[1]=p;state[0]=state[1]=MEM_COMMIT;reads=0;
        bool expected=!(p&(PAGE_GUARD|PAGE_NOCACHE|PAGE_WRITECOMBINE))&&((p&255)==4||(p&255)==8);
        assert(read_owned_data((HANDLE)1,0x1fe0,data,64,&got)==expected);
        assert(reads==(unsigned)expected&&got==(expected?64:0));
    }
    reads=0;prot[1]=PAGE_READWRITE;state[1]=0;
    assert(!read_owned_data((HANDLE)1,0x1fe0,data,64,&got)&&!reads);
    state[1]=MEM_COMMIT;fail_query=true;
    assert(!read_owned_data((HANDLE)1,0x1000,data,64,&got)&&!reads);
    fail_query=false;
    assert(!read_owned_data((HANDLE)1,UINTPTR_MAX-8,data,64,&got)&&!reads);
    fail_read=true;
    assert(!read_owned_data((HANDLE)1,0x1000,data,64,&got)&&reads==1);
    puts("NVENC_CAPTURE_READ_PASS all page-protection combinations, cross-region checks, overflow and short reads");
}
'''
    run_test(safety,'nvenc_capture_read')


if __name__=='__main__':main()
