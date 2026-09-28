/* Read-only recognition of bounded H.264 encoder method sequences.
 * Packet formats: supplied NVIDIA class/cl906f.h. Not a submission API and
 * not proof that a recognized host-memory sequence reached the hardware. */
#ifndef KESTREL_NVENC_METHOD_CAPTURE_H
#define KESTREL_NVENC_METHOD_CAPTURE_H
#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t method, data; } nvenc_capture_method;
typedef struct {
    uint32_t subchannel, count, words, application_id_observed;
    nvenc_capture_method methods[128];
} nvenc_capture_sequence;

static int nvenc_capture_start(uint32_t h, uint32_t next, size_t words) {
    uint32_t method=h&0x1fffu;
    if (method!=(0x200u>>2) && method!=(0x700u>>2))return 0;
    uint32_t op=h>>29, n=(h>>16)&0x1fffu;
    uint32_t value=op==4?n:next;
    if(method==(0x200u>>2)?value!=1:(value&15)!=3)return 0;
    return op==4 || ((op==1 || op==3 || op==5) && n==1 && words>=2);
}

static int nvenc_capture_decode(const uint32_t *p, size_t words,
                               nvenc_capture_sequence *out) {
    if (!p || !out || !words ||
        !nvenc_capture_start(p[0],words>1?p[1]:0,words)) return 0;
    nvenc_capture_sequence result={0};
    result.subchannel=(p[0]>>13)&7u;
    unsigned required=0;
    if(words>1024)words=1024;
    for(size_t at=0;at<words;) {
        uint32_t h=p[at++];
        if(!h)continue; /* documented NOP */
        uint32_t op=h>>29, n=(h>>16)&0x1fffu, sub=(h>>13)&7u;
        uint32_t method=(h&0xfffu)*4;
        if((h&0x1000u) || (op!=1 && op!=3 && op!=4 && op!=5))return 0;
        if(op!=4 && (!n || n>words-at))return 0;
        uint32_t count=op==4?1:n;
        /* Blackwell channel classes >C86F emit address[39:8], address[63:40]
         * as TWO non-incrementing words, including a zero high word. They are
         * one address, not two independent assignments. Linux 595 ELF 0x48ee0
         * and Windows 610.62 0x1800085d0 establish this packet form. */
        int address_pair=op==3 && n==2 && method>=0x70c && method<=0x74c;
        if(address_pair && p[at+1]>0xffffffu)return 0;
        int pair_nonzero=address_pair && (p[at] || p[at+1]);
        for(uint32_t i=0;i<count;i++) {
            uint32_t m=method+(op==1?i*4:op==5&&i?4:0);
            uint32_t data=op==4?n:p[at++];
            if(m>0x3ffc)return 0;
            if(sub!=result.subchannel)continue;
            if(result.count>=128)return 0;
            result.methods[result.count].method=m;
            result.methods[result.count++].data=data;
            if(address_pair && i==1)continue; /* retain upper word, validate once */
            switch(m) {
            case 0x200: if(data!=1 || result.count!=1)return 0; required|=1;break;
            case 0x700: if((data&15)!=3)return 0;required|=2;break;
            case 0x704: if(data)return 0;required|=4;break;
            case 0x710: if(!data && !pair_nonzero)return 0;required|=8;break;
            case 0x718: if(!data && !pair_nonzero)return 0;required|=16;break;
            case 0x71c: if(!data && !pair_nonzero)return 0;required|=32;break;
            case 0x734: if(!data && !pair_nonzero)return 0;required|=64;break;
            case 0x740: if(!data && !pair_nonzero)return 0;required|=128;break;
            case 0x744: if(!data && !pair_nonzero)return 0;required|=256;break;
            case 0x300:
                if((required&510)!=510)return 0;
                result.application_id_observed=required&1;
                result.words=(uint32_t)at;*out=result;return 1;
            default: break;
            }
        }
    }
    return 0; /* incomplete records never become evidence */
}
#endif
