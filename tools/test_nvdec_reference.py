#!/usr/bin/env python3
"""Actual reference verdict + actual SHA256, against archived Windows NVDEC.

Only the decoder boundary is mocked. The independently captured NV12 bytes are
never guessed/generated from input pixels. This is host verdict validation,
not evidence that Kestrel's hardware decoder completed this compressed frame.
"""
from pathlib import Path
from contextlib import ExitStack
import base64
import ctypes as C
import _ctypes
import hashlib
import json
import random
import re
import shutil
import subprocess
import tempfile
from test_gpu_stable_candidate import function
from decode_nvdec_reference import compare_kestrel

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tools/fixtures'


def array(text, name):
    body = text.split(name, 1)[1].split('{', 1)[1].split('}', 1)[0]
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))


def main():
    manifest = json.loads((FIXTURES / 'h264_nvdec_256_manifest.json').read_text())
    decoded = base64.b64decode(''.join((FIXTURES / 'h264_nvdec_256.nv12.b64').read_text().splitlines()), validate=True)
    encoded = array((FIXTURES / 'h264_nvenc_256.h').read_text(), 'h264_nvenc_256[]')
    oracle_header = (FIXTURES / 'h264_nvdec_256_oracle.h').read_text()
    y_hash = array(oracle_header, 'h264_nvdec_256_y_sha256')
    uv_hash = array(oracle_header, 'h264_nvdec_256_uv_sha256')
    assert len(decoded) == 98304 and len(encoded) == 5014
    assert hashlib.sha256(encoded).hexdigest() == manifest['source_sha256'] == 'efc9304e30db3db7923ec26a5c9ffd0487c565af4b23cab3e32f11e01dae46da'
    assert hashlib.sha256(decoded).hexdigest() == manifest['decoded_sha256'] == 'a782dd43fd91132e04632663728bcb984928d6f9c50507b5121faf1f78d12b3e'
    assert hashlib.sha256(decoded[:65536]).digest() == y_hash
    assert hashlib.sha256(decoded[65536:]).digest() == uv_hash
    assert y_hash.hex() == manifest['plane_sha256']['Y']
    assert uv_hash.hex() == manifest['plane_sha256']['UV']
    hw = manifest['hardware']
    assert hw['device'] == 'NVIDIA GeForce RTX 5070 Ti'
    assert (hw['coded_width'], hw['coded_height'], hw['output_pitch'], hw['output_bytes']) == (256, 256, 256, 98304)
    assert (hw['decode_status'], hw['sequences'], hw['pictures'], hw['displays']) == (2, 1, 1, 1)
    assert len(hw['api_calls']) == 24 and all(call['status'] == 0 for call in hw['api_calls'])
    assert manifest['configuration']['scaling'] is False
    assert manifest['configuration']['creation_flags'] == 'cudaVideoCreate_PreferCUVID=4'
    # Static wiring audit: independent verdict, known decoder first, and no
    # additional codec requirement in desktop admission.
    gpu = (ROOT / 'kernel/gpu.c').read_text()
    boot = function(gpu, 'gpu_boot_and_log')
    order = ['if (cmdline_has("gpustart") && nvrm_is_ready())',
             'nvdec = nv_nvdec_selftest_hw();',
             'if (nvdec == 0) nvdec_app = nv_nvdec_application_selftest_hw();',
             'nvenc = nv_nvenc_selftest_hw();', 'codec_roundtrip = nv_nvenc_roundtrip_hw();',
             'post_ok = nvkms_kapi_run_visible_accel_test']
    assert [boot.index(x) for x in order] == sorted(boot.index(x) for x in order)
    gate = re.search(r'gpu_test_gui_ready\s*=\s*([^;]+);', boot)[1]
    assert re.sub(r'\s+', '', gate) == ('kapi_ok&&kr.scanout_confirmed&&host_tree&&'
        'copy==0&&compute==0&&raster3d==0&&pre_ok&&post_ok&&runtime_ok&&runtime_accel_ok')
    main_src = (ROOT / 'kernel/main.c').read_text()
    assert re.search(r'bool gpu_test_boot = cmdline_has\("gpustart"\);[\s\S]*?'
                     r'if \(gpu_test_boot\) \{[^}]*gpu_boot_test_sync\(\);', main_src)
    crypto = (ROOT / 'kernel/crypto.c').read_text()
    crypto_header = (ROOT / 'kernel/crypto.h').read_text()
    reference = (ROOT / 'kernel/nv_video_reference.c').read_text()
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define SHA256_SIZE 32
'''
    end = crypto_header.index('} sha256_t;') + len('} sha256_t;')
    c += crypto_header[crypto_header.rfind('typedef struct {', 0, end):end] + '\n'
    c += re.search(r'static inline u32 word\([^}]+}', crypto)[0] + '\n'
    c += '#define sha256 production_sha256\n'
    c += crypto[crypto.index('static const u32 sha256_k'):crypto.index('/* ------------------------------------------------------------------- HMAC */')]
    c += '\n#undef sha256\n'
    for path in ('include/kestrel/video.h', 'tools/fixtures/h264_nvenc_256.h',
                 'tools/fixtures/h264_nvdec_256_oracle.h'):
        c += '#include "' + (ROOT / path).as_posix() + '"\n'
    c += r'''
static const u8 *pixels;
static u8 *decoded_pointer;
static unsigned copied,guard,hash_calls,decode_calls,warnings,infos;
static char last_log[768];
static void info_log(const char *tag,const char *fmt,...){
    assert(!strcmp(tag,"nv-video"));infos++;
    /* Match the real bounded kernel entry, then accumulate separate lines. */
    char entry[168];
    va_list args;va_start(args,fmt);int n=vsnprintf(entry,sizeof entry,fmt,args);va_end(args);
    assert(n>=0 && (unsigned)n<sizeof entry);
    size_t used=strlen(last_log);
    assert(used+(unsigned)n+2<sizeof last_log);
    memcpy(last_log+used,entry,(size_t)n);
    last_log[used+n]='\n';last_log[used+n+1]=0;
}
static void warn_log(const char *tag,const char *fmt,...){(void)tag;(void)fmt;warnings++;}
#define kinfo info_log
#define kwarn warn_log
static void sha256(const void *p,size_t n,u8 out[32]){
    assert(hash_calls<2);
    assert(p==decoded_pointer+(hash_calls?65536:0));
    assert(n==(hash_calls?32768u:65536u));hash_calls++;
    production_sha256(p,n,out);
}
static int nv_nvdec_decode_idr(const u8 *stream,unsigned bytes,u8 *output,
                               unsigned capacity,kvideo_request_t *info){
    decode_calls++;
    assert(stream==h264_nvenc_256 && bytes==sizeof h264_nvenc_256 && bytes==5014);
    assert(capacity==98304 && copied<=capacity);
    for(unsigned i=0;i<capacity;i++)assert(output[i]==0xa5);
    kvideo_request_t expected={.version=KVIDEO_ABI,.operation=KVIDEO_H264_DECODE_IDR};
    assert(!memcmp(info,&expected,sizeof expected));
    decoded_pointer=output;memcpy(output,pixels,copied);
    info->phase=KVIDEO_PHASE_COMPLETE;info->decoded_mbs=256;
    info->required_bytes=info->written_bytes=98304;
    info->coded_width=info->coded_height=info->display_width=info->display_height=info->pitch=256;
    switch(guard){
    case 1:return -5;
    case 2:info->phase=KVIDEO_PHASE_RELEASE;break;
    case 3:info->parse_status=1;break;
    case 4:info->firmware_error=1;break;
    case 5:info->slice_error=1;break;
    case 6:info->error_mbs=1;break;
    case 7:info->decoded_mbs=255;break;
    case 8:info->required_bytes=98303;break;
    case 9:info->written_bytes=98303;break;
    case 10:info->coded_width=255;break;
    case 11:info->coded_height=255;break;
    case 12:info->display_width=255;break;
    case 13:info->display_height=255;break;
    case 14:info->pitch=512;break;
    case 15:info->crop_left=1;break;
    case 16:info->crop_right=1;break;
    case 17:info->crop_top=1;break;
    case 18:info->crop_bottom=1;break;
    case 19:info->decoded_mbs=257;break;
    case 20:info->written_bytes=98305;break;
    case 21:info->required_bytes=98305;break;
    }
    return 0;
}
'''
    c += function(reference, 'nv_video_digest_hex') + '\n'
    c += function(reference, 'nv_nvdec_application_selftest_hw') + '\n'
    c += r'''
__declspec(dllexport) int run(const u8 *data,unsigned n,unsigned fault){
    pixels=data;copied=n;guard=fault;hash_calls=decode_calls=warnings=infos=0;last_log[0]=0;
    int rc=nv_nvdec_application_selftest_hw();assert(decode_calls==1);
    if(fault){assert(rc==1 && !hash_calls && warnings==1 && !infos);}
    else{assert(hash_calls==2 && !warnings && infos==3);}
    return rc;
}
__declspec(dllexport) void digest(const u8 *data,size_t n,u8 *out){production_sha256(data,n,out);}
__declspec(dllexport) const char *log_text(void){return last_log;}
'''
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    count = 0
    with tempfile.TemporaryDirectory(prefix='kestrel-nvdec-reference-') as tmp, ExitStack() as cleanup:
        captured=json.loads((FIXTURES/'h264_nvdec_256_context.json').read_text())
        assert captured['source_sha256']==manifest['source_sha256']
        assert captured['decoded_sha256']==manifest['decoded_sha256']
        assert captured['hardware']['decode_status']==2
        assert all(call['status']==0 for call in captured['hardware']['api_calls'])
        # Compare TODAY'S actual parser/picture builder, not the saved native
        # comparison. Expected semantic fields came from successful CUVID.
        context=compare_kestrel(tmp,encoded,captured['hardware']['cuvid_picture'])
        assert not context['mismatches'],context['mismatches']
        assert context['cuvid_used_dpb_entries']==0
        assert all(not any(p.values()) for p in context['dpb'])
        # CUVID's callback strips parameter NALs and one start-code zero only.
        transport=captured['parser_transport']
        assert transport['bytes']==4981 and transport['slice_offsets']==[0]
        assert hashlib.sha256(encoded[33:]).hexdigest()==transport['sha256']
        src, dll = Path(tmp) / 'reference.c', Path(tmp) / 'reference.dll'
        src.write_text(c)
        subprocess.run([clang, '-shared', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fsanitize-trap=all', str(src), '-o', str(dll)], check=True)
        lib = C.CDLL(str(dll));cleanup.callback(_ctypes.FreeLibrary,lib._handle)
        lib.run.argtypes = [C.c_void_p,C.c_uint,C.c_uint];lib.run.restype=C.c_int
        lib.digest.argtypes = [C.c_void_p,C.c_size_t,C.c_void_p]
        lib.log_text.restype = C.c_char_p
        def check(data,success=False,fault=0,copied=None):
            nonlocal count
            count+=1
            rc=lib.run(C.create_string_buffer(data),len(data) if copied is None else copied,fault)
            assert rc==(0 if success else 1),(count,fault,copied)
            if not fault:
                text=lib.log_text().decode()
                assert ('reference PASS:' in text)==success
                assert ('reference FAIL:' in text)!=success
                actual=data if copied is None else data[:copied]+b'\xa5'*(98304-copied)
                assert 'Y-sha256='+hashlib.sha256(actual[:65536]).hexdigest() in text
                assert 'UV-sha256='+hashlib.sha256(actual[65536:]).hexdigest() in text
        check(decoded,True)
        # Stale success cannot survive failed status, incomplete copy, or poison.
        for fault in range(1,22):
            check(decoded,fault=fault);check(decoded,fault=fault,copied=0)
        for copied in (0,1,255,256,65535,65536,65537,98303):check(decoded,copied=copied)
        y,uv=decoded[:65536],decoded[65536:]
        corrupted=[uv+y, y[:256]*256+uv, y+uv[:256]*128,
                   b''.join(y[row*256:(row+1)*256] for row in reversed(range(256)))+uv,
                   y+b''.join(uv[row*256:(row+1)*256] for row in reversed(range(128))),
                   decoded[1:]+decoded[:1],bytes(reversed(decoded)),
                   y+b''.join(uv[i:i+2][::-1] for i in range(0,len(uv),2))]
        for data in corrupted:assert data!=decoded;check(data)
        rng=random.Random(0xdec256)
        for index in [0,255,256,65535,65536,65537,98303]+[rng.randrange(98304) for _ in range(32)]:
            data=bytearray(decoded);data[index]^=1;check(bytes(data))
        # Reuse the same static output after each bad frame; it is re-poisoned.
        check(decoded,True)
        vectors=[b'',b'abc',b'a'*55,b'a'*56,b'a'*63,b'a'*64,b'a'*65,encoded,y,uv,decoded]
        vectors += [rng.randbytes(n) for n in (1,17,127,128,129,1024,4097)]
        for data in vectors:
            out=C.create_string_buffer(32);lib.digest(C.create_string_buffer(data),len(data),out)
            assert out.raw==hashlib.sha256(data).digest()
        print(f'PASS actual NVDEC reference verdict: {count} oracle/status/pixel-corruption cases, {len(vectors)} actual SHA256/hashlib comparisons; actual context matches independent CUVID fields/scaling/DPB; boot order and independent desktop gate preserved (host validation only)')


if __name__ == '__main__':
    main()
