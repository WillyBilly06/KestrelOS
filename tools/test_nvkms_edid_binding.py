"""Explicit zero-identity monitor bindings: production parser, no guessed sink."""
from test_gpu_stable_candidate import ROOT, run_test

def main():
    source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvkms_edid_binding.h"
static int lookup(const char *s,unsigned h,unsigned *p){return nvkms_edid_binding(s,(unsigned)strlen(s),h,p);}
int main(void){
    unsigned p=99;
    unsigned char edid[128]={0,255,255,255,255,255,255,0};
    edid[8]=4;edid[18]=1;
    unsigned sum=0;for(unsigned i=0;i<127;i++)sum+=edid[i];edid[127]=(unsigned char)-sum;
    assert(nvkms_edid_has_identity(edid,128));
    assert(!nvkms_edid_has_identity(edid,127));
    edid[12]^=1;assert(!nvkms_edid_has_identity(edid,128));
    assert(lookup("00000800 00000200\n",0x800,&p)&&p==0x200);
    p=99;assert(!lookup("00000800 00000200\n",0x200,&p)&&p==99);
    assert(!lookup("00000800 00000200\n",0x2000,&p));
    assert(lookup("00000800 00000200\r\n00002000 000000aa\n",0x2000,&p)&&p==0xaa);
    assert(!lookup("00000800 00000200\n00000800 000000aa\n",0x800,&p));
    assert(!lookup("00000800 00000200\nmalformed",0x800,&p));
    assert(!lookup("00000800 00000200bad",0x800,&p));
    assert(!lookup("00000800 00000000",0x800,&p));
    const char *record="00000800 00000200";
    for(unsigned n=0;n<17;n++)assert(!nvkms_edid_binding(record,n,0x800,&p));
    assert(!nvkms_edid_binding(record,513,0x800,&p));
    puts("PASS explicit EDID bindings: moved cable, old/unbound paths, CRLF, duplicate/invalid/truncated refusal");
}
'''
    run_test(source,'nvkms-edid-binding',[ROOT/'kernel'])
    binding=(ROOT/'firmware/edid/nvkms-bindings.txt').read_text()
    assert binding.strip()=='00000800 00000200'

if __name__=='__main__':main()
