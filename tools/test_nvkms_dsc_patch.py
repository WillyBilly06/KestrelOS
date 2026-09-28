#!/usr/bin/env python3
"""Execute the vendor's actual DSC method builder on host-memory buffers.

No GPU, monitor, codec, MMIO or VM is used. Checks the full patched machine
function, not a rewritten arithmetic model. Native hardware visibility is
outside this test's scope. Unexpected external calls hit an UD2 trap.
"""
import hashlib
import io
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from patch_nvkms_dsc import INPUT_SHA256, PATCH_OFFSET, PATCHES, patch_dsc
from test_gpu_stable_candidate import ROOT, run_test


def main():
    original = (ROOT / 'Linux NVIDIA Driver/kernel-open/nvidia-modeset/nv-modeset-kernel.o_binary').read_bytes()
    # Reproduce only the already-established EDID derivative in memory.
    import patch_nvkms_edid_override as edid
    assert hashlib.sha256(original).hexdigest() == edid.SOURCE_SHA256
    before = bytearray(original)
    base, _ = edid.elf64_section(original, edid.SECTION_NAME)
    for off, old, new in ((edid.BRANCH_OFFSET, edid.ORIGINAL, edid.PATCHED),
                          (edid.LIVE_PRESERVE_OFFSET, edid.LIVE_PRESERVE_ORIGINAL, edid.LIVE_PRESERVE_PATCHED),
                          (edid.LIVE_BRIDGE_OFFSET, edid.LIVE_BRIDGE_ORIGINAL, edid.LIVE_BRIDGE_PATCHED)):
        assert before[base+off:base+off+len(old)] == old
        before[base+off:base+off+len(old)] = new
    before = bytes(before)
    assert hashlib.sha256(before).hexdigest() == INPUT_SHA256
    after = patch_dsc(before)
    ef = ELFFile(io.BytesIO(before))
    patched = ELFFile(io.BytesIO(after))
    allowed = set()
    arrays = []
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    for number, (name, old, new) in enumerate(PATCHES):
        s = ef.get_section_by_name(name)
        p = patched.get_section_by_name(name)
        start = s['sh_offset'] + PATCH_OFFSET
        allowed.update(range(start, start + len(old)))
        assert s.data()[PATCH_OFFSET:PATCH_OFFSET+len(old)] == old
        assert p.data()[PATCH_OFFSET:PATCH_OFFSET+len(new)] == new
        # Exact instruction boundaries: no branches enter the overwritten body.
        instructions = list(decoder.disasm(s.data(), 0))
        assert any(i.address == PATCH_OFFSET for i in instructions)
        assert any(i.address == PATCH_OFFSET + len(old) for i in instructions)
        for ins in instructions:
            if ins.mnemonic.startswith('j') and ins.op_str.startswith('0x'):
                target = int(ins.op_str, 16)
                assert not PATCH_OFFSET < target < PATCH_OFFSET + len(old)
            # The old local spill really is dead, including paths after DP.
            if 'rsp + 0xc' in ins.op_str:
                assert ins.mnemonic == 'mov' and ins.op_str.startswith('dword ptr [rsp + 0xc],')
        for variant, section in (('old', s), ('new', p)):
            code = bytearray(section.data())
            trap = len(code)
            code += bytes.fromhex('0f 0b')
            relocs = ef.get_section_by_name('.rela' + name)
            for r in relocs.iter_relocations():
                off = r['r_offset']
                assert r['r_info_type'] == 2 and code[off - 1] == 0xe8
                struct.pack_into('<i', code, off, trap - (off + 4))
            arrays.append(f'static const unsigned char {variant}{number}[]={{' +
                          ','.join(str(b) for b in code) + '};')
    assert len(before) == len(after)
    assert all(a == b or i in allowed for i, (a, b) in enumerate(zip(before, after)))
    # Fail closed on vendor input, already-patched input, truncation, corruption.
    for bad in (original, after, before[:-1], b'', before[:100] + bytes([before[100] ^ 1]) + before[101:]):
        try:
            patch_dsc(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted unknown DSC input')
    with tempfile.TemporaryDirectory(prefix='kestrel-dsc-patch-') as tmp:
        dest = Path(tmp) / 'derived.o'
        subprocess.run([sys.executable, str(ROOT/'tools/patch_nvkms_edid_override.py'),
                        str(ROOT/'Linux NVIDIA Driver/kernel-open/nvidia-modeset/nv-modeset-kernel.o_binary'),
                        str(dest)], check=True)
        assert dest.read_bytes() == after
    source = r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void (__attribute__((sysv_abi)) *builder)(void *, unsigned, void *, unsigned);
static builder load(const unsigned char *data, size_t n) {
    void *p=VirtualAlloc(NULL,n,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(p); memcpy(p,data,n); DWORD old;
    assert(VirtualProtect(p,n,PAGE_EXECUTE_READ,&old));
    assert(FlushInstructionCache(GetCurrentProcess(),p,n));
    return (builder)p;
}
static void put32(unsigned char *p, size_t off, uint32_t v){memcpy(p+off,&v,4);}
static void putptr(unsigned char *p,size_t off,void *v){memcpy(p+off,&v,sizeof v);}
static unsigned capture(builder f,unsigned head,unsigned bpc,unsigned bpp,
                        unsigned mode,unsigned type,uint32_t out[256]) {
    unsigned char display[16]={0},dev[0x318]={0},channel[0xc8]={0},fifo[0x600]={0};
    unsigned char dsc[0xb0]={0};
    putptr(display,8,dev); putptr(dev,0x310,channel); putptr(channel,0xc0,fifo);
    putptr(channel,0xa0,out); put32(channel,0xb8,256); put32(channel,0x54,1);
    put32(dsc,0,bpp);
    // Distinct PPS payload words: verifies the whole stream is preserved.
    for(unsigned i=0;i<32;i++)put32(dsc,4+4*i,0xabcdef00u+i);
    put32(dsc,4,(bpc<<28)|0x09000012u);
    put32(dsc,0x84,mode); put32(dsc,0xa4,type);
    memset(out,0xa5,256*sizeof *out);
    f(display,head,dsc,0);
    uint32_t *end; memcpy(&end,channel+0xa0,sizeof end);
    assert(end>=out&&end<=out+256);
    unsigned n=(unsigned)(end-out);
    for(unsigned i=n;i<256;i++)assert(out[i]==0xa5a5a5a5u);
    return n;
}
''' + '\n'.join(arrays) + r'''
int main(void) {
    builder original[2]={load(old0,sizeof old0),load(old1,sizeof old1)};
    builder patched[2]={load(new0,sizeof new0),load(new1,sizeof new1)};
    uint32_t a[256],b[256]; unsigned cases=0,changed=0;
    for(unsigned hal=0;hal<2;hal++)for(unsigned head=0;head<4;head++)
    for(unsigned bpc=8;bpc<=12;bpc+=2)for(unsigned bpp=0;bpp<1024;bpp++)
    for(unsigned mode=1;mode<=2;mode++) {
        unsigned n=capture(original[hal],head,bpc,bpp,mode,3,a);
        assert(n==70&&capture(patched[hal],head,bpc,bpp,mode,3,b)==n);
        assert(b[0]==(0x40000u|((0x22d4u+(head<<(hal==1?11:10)))&0xfffcu)));
        unsigned flags=hal==1?0x31u:(mode==1?0x33u:0x31u);
        unsigned expected=2u<<(bpc-8);
        assert(b[1]==(flags|(expected<<6)));
        unsigned oldThreshold=(2u<<((bpp-8u)&31u))&0x3ffu;
        assert(a[1]==(flags|(oldThreshold<<6)));
        for(unsigned i=0;i<n;i++)if(i!=1)assert(a[i]==b[i]);
        if(a[1]!=b[1])changed++;
        // PPS data words and SDP header remain exact, including BPC.
        assert(b[5]==((bpc<<28)|0x09000012u));
        for(unsigned i=1;i<32;i++)assert(b[5+2*i]==0xabcdef00u+i);
        assert(b[69]==0x7f1000u);
        cases++;
    }
    for(unsigned hal=0;hal<2;hal++)for(unsigned head=0;head<4;head++) {
        unsigned n=capture(original[hal],head,8,128,1,0,a);
        assert(n==4&&capture(patched[hal],head,8,128,1,0,b)==n);
        assert(!memcmp(a,b,n*4)&&!b[1]&&!b[3]);
    }
    assert(changed>0);
    printf("PASS actual vendor DP DSC builders: %u cases, %u corrected controls; PPS/SDP/disable unchanged; no GPU access\n",cases,changed);
}
'''
    run_test(source, 'nvkms_dsc_patch')
    print('PASS exact-byte patch, input rejection, branch boundaries, dead spill, relocation exclusion and build-stage integration')
    print('Derived NVKMS SHA256:', hashlib.sha256(after).hexdigest())


if __name__ == '__main__':
    main()
