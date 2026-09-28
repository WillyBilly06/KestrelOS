#!/usr/bin/env python3
"""Execute production override/evidence/report code with mocked AUX edges.

No GPU emulation, link-training proof, or codec execution.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    client = (ROOT / 'kernel/nvkms_kapi_client.c').read_text()
    header = (ROOT / 'kernel/nvkms_kapi_client.h').read_text()
    gpu = (ROOT / 'kernel/gpu.c').read_text()
    evidence = re.search(r'typedef struct \{\n    u32 handle, connector.*?nvkms_display_evidence_t;', header, re.S)[0]
    capacity = re.search(r'^#define KESTREL_NVKMS_EDID_BYTES .*', header, re.M)[0]
    macro = gpu[gpu.index('    #define GLINE('):gpu.index('\n\n', gpu.index('    #define GLINE('))]
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvkms-kapi.h"
#include "nvkms_edid_binding.h"
typedef uint32_t u32; typedef uint8_t u8;
''' + capacity + '\n' + evidence + r'''
static struct NvKmsKapiDevice *test_device;
static struct { char manufacturer[4],model[16]; } parsed_edid;
static unsigned loads,queries;
static bool loaded,matching,accepted,connected,parsed;
static bool alloc_failed;
static unsigned allocations;
static void *kzalloc(size_t n){if(alloc_failed)return NULL;allocations++;return calloc(1,n);}
static void kfree(void *p){assert(p&&allocations);allocations--;free(p);}
static void strlcpy(char *d,const char *s,size_t n){if(n)snprintf(d,n,"%s",s);}
#define kwarn(...) ((void)0)
static void make_edid(NvU8 *p){
    memset(p,0,128);for(unsigned i=1;i<7;i++)p[i]=255;
    p[8]=4;p[9]=0x72;p[18]=1;
    unsigned sum=0;for(unsigned i=0;i<127;i++)sum+=p[i];p[127]=(NvU8)-sum;
}
static NvBool load_provisioned_edid(NvU32 h,NvU8 *p,NvU16 *n){
    assert(h==0x800);loads++;make_edid(p);*n=128;
    if(!matching)p[8]^=1;
    return loaded;
}
static NvBool dynamic(struct NvKmsKapiDevice *d,struct NvKmsKapiDynamicDisplayParams *p){
    assert(d==test_device&&p->handle==0x800&&p->overrideEdid);
    assert(!p->forceConnected&&!p->forceDisconnected);
    queries++;p->connected=connected;return accepted;
}
static NvBool edid_parse(const NvU8 *p,NvU32 n,void *out){
    assert(p&&n==128&&out==&parsed_edid);
    strlcpy(parsed_edid.manufacturer,"ACR",4);strlcpy(parsed_edid.model,"Test panel",16);
    return parsed;
}
static struct { NvBool (*getDynamicDisplayInfo)(struct NvKmsKapiDevice *,struct NvKmsKapiDynamicDisplayParams *); } kapi={dynamic};
''' + function(client, 'apply_provisioned_edid') + '\n' + function(client, 'capture_display_evidence') + r'''
static struct NvKmsKapiDynamicDisplayParams live,saved;
static void reset(void){
    assert(!allocations);alloc_failed=false;
    loads=queries=0;loaded=matching=accepted=connected=parsed=true;
    memset(&live,0,sizeof live);live.handle=0x800;live.connected=true;
    live.edid.bufferSize=128;make_edid(live.edid.buffer);saved=live;
}
static void refused(void){
    NvBool valid=false;
    assert(!apply_provisioned_edid(0x800,&live,&valid));
    assert(!valid&&!memcmp(&live,&saved,sizeof live)&&!allocations);
}
static void report_bounds(void){
    struct {char rep[64];unsigned guard;} storage={.guard=0x12345678};
    #define rep storage.rep
    int n=0;
''' + macro + r'''
    GLINE("%s", "short");assert(n==5);
    GLINE("%080d",1);assert(n==63&&strlen(rep)==63);
    for(unsigned i=0;i<1000;i++)GLINE("overflow %u",i);
    assert(n==63&&storage.guard==0x12345678&&rep[63]==0);
    #undef rep
    #undef GLINE
}
int main(void){
    reset();live.connected=false;saved=live;refused();assert(!loads&&!queries);
    reset();loaded=false;refused();assert(loads==1&&!queries);
    reset();alloc_failed=true;refused();assert(!loads&&!queries);
    reset();matching=false;refused();assert(!queries);
    reset();accepted=false;refused();assert(queries==1);
    reset();connected=false;refused();
    reset();parsed=false;refused();
    reset();NvBool valid=false;
    assert(apply_provisioned_edid(0x800,&live,&valid)&&valid&&live.overrideEdid&&live.connected);
    assert(!allocations);
    nvkms_display_evidence_t out;
    capture_display_evidence(&out,0x800,7,true,&live,true,true,1000);
    assert(out.handle==0x800&&out.connector==7&&out.is_dp&&out.source==2&&out.waited_ms==1000);
    assert(out.size==128&&!memcmp(out.bytes,live.edid.buffer,128)&&!strcmp(out.manufacturer,"ACR"));
    memset(&live.edid,0,sizeof live.edid);
    capture_display_evidence(&out,0x200,8,false,&live,false,false,12);
    assert(!out.size&&!out.source&&!out.manufacturer[0]&&!out.model[0]&&!out.is_dp);
    for(unsigned i=0;i<sizeof out.bytes;i++)assert(!out.bytes[i]);
    live.edid.bufferSize=sizeof live.edid.buffer;memset(live.edid.buffer,0xa5,sizeof live.edid.buffer);
    capture_display_evidence(&out,0x200,8,true,&live,true,false,0);
    assert(out.size==NVKMS_KAPI_EDID_BUFFER_SIZE&&out.source==1&&out.bytes[out.size-1]==0xa5);
    live.edid.bufferSize=0xffff;
    capture_display_evidence(&out,0x200,8,true,&live,false,false,0);
    assert(out.size==sizeof out.bytes&&!out.source);
    report_bounds();
    puts("PASS production EDID override rollback, evidence/provenance/full ABI capacity, bounded report appends");
}
'''
    nv = ROOT / 'out/nvidia-open-595.99.02'
    run_test(code, 'nvkms_display_evidence', [
        ROOT / 'kernel', nv / 'src/nvidia-modeset/kapi/interface',
        nv / 'src/nvidia-modeset/interface', nv / 'src/common/sdk/nvidia/inc',
        nv / 'kernel-open/common/inc'])


if __name__ == '__main__':
    main()
