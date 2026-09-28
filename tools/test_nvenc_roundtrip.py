#!/usr/bin/env python3
"""Actual native roundtrip wrapper, modeled canonical getter/decoder boundaries.
Checks complete-stream identity and strict metadata/full-frame gates. Does not
claim actual entropy decoding, encoding, or native GPU execution.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_gpu_stable_candidate import function
import gen_h264_iframe as fixture

ROOT = Path(__file__).resolve().parents[1]


def main():
    saved = fixture.WIDTH_MBS, fixture.HEIGHT_MBS, fixture.WIDTH, fixture.HEIGHT
    try:
        fixture.WIDTH_MBS = fixture.HEIGHT_MBS = 16; fixture.WIDTH = fixture.HEIGHT = 256
        data = fixture.nal(3, 7, fixture.sps()) + fixture.nal(3, 8, fixture.pps()) + fixture.nal(3, 5, fixture.idr_slice())
        fixture.parse_validate(data)  # Every actual I_PCM macroblock/sample.
        # The original fixed level 1.1 is too small for 256 macroblocks. The
        # test's SPS uses level 3.1; all picture syntax/samples stay identical.
        data = data[:7] + bytes([31]) + data[8:]
    finally:
        fixture.WIDTH_MBS, fixture.HEIGHT_MBS, fixture.WIDTH, fixture.HEIGHT = saved
    src = (ROOT / 'kernel/nv_chan.c').read_text()
    production = function(src, 'nv_nvenc_roundtrip_hw')
    assert 'nv_nvenc_test_stream(' in production and 'nv_nvdec_decode_idr(' in production
    assert 'ensure_nvdec_roundtrip_vram' not in src
    code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
extern void *memcpy(void *,const void *,size_t);
extern void *memset(void *,int,size_t);
extern int memcmp(const void *,const void *,size_t);
extern int printf(const char *,...);
extern void exit(int);
extern void record(const char *,const char *,...);
void __main(void) {}
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
static unsigned scenario,getters,decodes,cases,pixel_bad;
unsigned passes;
#define assert(x) ((x)?(void)0:(printf("scenario %u line %d: %s\n",scenario,__LINE__,#x),exit(1)))
#define kinfo(...) record(__VA_ARGS__)
#define kwarn(...) record(__VA_ARGS__)
#define kerr(...) record(__VA_ARGS__)
typedef struct {bool open,submit_failed;} nv_channel_t;
#define CH_NVDEC 0
static nv_channel_t channels[1];
'''
    for name in ('kernel/nvenc_h264_stream.h', 'include/kestrel/video.h'):
        code += '#include "' + (ROOT / name).as_posix() + '"\n'
    code += 'static const u8 reference[]={\n' + ','.join(map(str, data)) + '\n};\n'
    code += r'''
static bool nv_nvenc_test_stream(u8 *out,u32 cap,u32 *bytes,kh264_decode_desc *desc){
    getters++;assert(getters==1 && !decodes && out && bytes && desc);
    assert(cap==NVENC_H264_STREAM_CAPACITY && cap>=sizeof reference);
    if(scenario==3){*bytes=0;memset(desc,0,sizeof *desc);return false;}
    memcpy(out,reference,sizeof reference);*bytes=sizeof reference;
    assert(kh264_parse_annexb(out,*bytes,desc)==KH264_OK);
    assert(desc->coded_width==256 && desc->coded_height==256 && desc->slice_end==sizeof reference);
    switch(scenario){
    case 50:desc->coded_width=240;break;
    case 51:desc->coded_height=240;break;
    case 52:desc->display_width=240;break;
    case 53:desc->display_height=240;break;
    case 54:desc->crop_left=2;break;
    case 55:desc->crop_right=2;break;
    case 56:desc->crop_top=2;break;
    case 57:desc->crop_bottom=2;break;
    }
    return true;
}
static int nv_nvdec_decode_idr(const u8 *input,u32 bytes,u8 *output,u32 capacity,kvideo_request_t *info){
    decodes++;assert(getters==1 && decodes==1 && input && output && info);
    assert(bytes==sizeof reference && !memcmp(input,reference,sizeof reference));
    assert(capacity==98304 && info->version==1 && info->operation==1);
    assert((uintptr_t)output+capacity<=(uintptr_t)input || (uintptr_t)output>=(uintptr_t)input+bytes);
    for(unsigned i=0;i<capacity;i++)assert(output[i]==0xa5);
    if(scenario!=29)memset(output,128,capacity);
    info->coded_width=info->coded_height=info->display_width=info->display_height=256;
    info->crop_left=info->crop_right=info->crop_top=info->crop_bottom=0;
    info->pitch=256;info->required_bytes=info->written_bytes=98304;info->phase=10;
    info->parse_status=info->firmware_error=info->slice_error=info->error_mbs=0;info->decoded_mbs=256;
    switch(scenario){
    case 4:return -5;
    case 5:info->phase=9;break;
    case 6:info->required_bytes--;break;
    case 7:info->required_bytes++;break;
    case 8:info->written_bytes--;break;
    case 9:info->written_bytes++;break;
    case 10:info->coded_width=240;break;
    case 11:info->coded_height=240;break;
    case 12:info->display_width=240;break;
    case 13:info->display_height=240;break;
    case 14:info->crop_left=2;break;
    case 15:info->crop_right=2;break;
    case 16:info->crop_top=2;break;
    case 17:info->crop_bottom=2;break;
    case 18:info->pitch=255;break;
    case 19:info->parse_status=1;break;
    case 20:info->firmware_error=0x123;break;
    case 21:info->slice_error=0x456;break;
    case 22:info->decoded_mbs=255;break;
    case 23:info->decoded_mbs=257;break;
    case 24:info->error_mbs=1;break;
    case 25:output[0]=0;break;
    case 26:output[65535]=0;break;
    case 27:output[65536]=0;break;
    case 28:output[98303]=0;break;
    case 30:memset(info,0,sizeof *info);break;
    case 31:info->required_bytes=info->written_bytes=0xffffffffu;break;
    case 32:info->pitch=0xffffffffu;break;
    case 33:info->required_bytes=info->written_bytes=0;break;
    case 34:return 1;
    case 40:assert(pixel_bad<capacity);output[pixel_bad]=129;break;
    }
    return 0;
}
'''
    code += production + '\n' + r'''
static void run(unsigned s){
    scenario=s;getters=decodes=passes=0;cases++;
    channels[0]=(nv_channel_t){.open=s!=1,.submit_failed=s==2};
    int rc=nv_nvenc_roundtrip_hw();
    assert((rc==0)==(s==0));assert(passes==(s==0));
    assert(getters==(s!=1 && s!=2));assert(decodes==(s!=1 && s!=2 && s!=3 && s<50));
}
int main(void){
    for(unsigned s=0;s<=34;s++)run(s);
    for(unsigned s=50;s<=57;s++)run(s);
    /* First/last pixels, every row boundary in both planes, and interiors.
     * A single wrong byte must suppress PASS, including the final chroma byte. */
    for(unsigned row=0;row<384;row++){
        pixel_bad=row*256;run(40);pixel_bad=row*256+255;run(40);
    }
    for(unsigned i=0;i<127;i++){pixel_bad=(i*7727u+193u)%98304u;run(40);}
    run(0); // Prior failures cannot taint the next explicitly successful decode.
    printf("NVENC-to-native-NVDEC wrapper PASS: %u cases, full stream identity, exact metadata/status and Y/UV rejection gates; modeled codecs, no native proof\n",cases);
    return 0;
}
'''
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    with tempfile.TemporaryDirectory(prefix='kestrel-roundtrip-') as tmp:
        c, obj, exe = (Path(tmp) / name for name in ('test.c', 'test.obj', 'test.exe'))
        c.write_text(code)
        subprocess.run([clang, '--target=x86_64-w64-windows-gnu', '-mno-ms-bitfields',
                        '-ffreestanding', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fsanitize-trap=all', '-c', str(c), '-o', str(obj)], check=True)
        # Build the printf bridge with the host CRT's headers/inlines. The
        # production ABI translation unit still uses Linux bitfield packing.
        bridge = Path(tmp) / 'log.c'
        bridge.write_text('#include <stdio.h>\n#include <stdarg.h>\n#include <string.h>\n'
                          'extern unsigned passes;\n'
                          'void record(const char *component,const char *fmt,...){\n'
                          '(void)component;char line[1024];va_list ap;va_start(ap,fmt);'
                          'vsnprintf(line,sizeof line,fmt,ap);va_end(ap);'
                          'if(strstr(line,"PASS"))passes++;}\n')
        subprocess.run([clang, str(obj), str(bridge), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__': main()
