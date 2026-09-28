#!/usr/bin/env python3
"""Execute the actual kernel IDR entrypoint with modeled hardware boundaries.

This tests transport/control flow, not entropy decoding or GPU execution. Only
the 64x64 fixture is a complete compressed picture; varied synthetic pictures
carry real SPS/PPS/slice headers and intentionally model the decoder payload.
Reference pixels use NIL's independent address-to-coordinate sector lists, not
a production linear->tiled conversion followed by its inverse.
"""
from pathlib import Path
from contextlib import ExitStack
import ctypes as C
import _ctypes
import random
import re
import shutil
import subprocess
import tempfile
from test_gpu_stable_candidate import function
from test_h264_decode import stream

ROOT = Path(__file__).resolve().parents[1]


class Request(C.Structure):
    _fields_ = [('version', C.c_uint), ('operation', C.c_uint)] + [
        (n, C.c_ulonglong) for n in ('input', 'input_bytes', 'output', 'output_capacity')] + [
        (n, C.c_uint) for n in ('coded_width', 'coded_height', 'display_width', 'display_height',
        'crop_left', 'crop_top', 'crop_right', 'crop_bottom', 'pitch', 'required_bytes',
        'written_bytes', 'phase', 'parse_status', 'firmware_error', 'slice_error',
        'decoded_mbs', 'error_mbs', 'reserved')]


def reference_maps():
    copy = (ROOT / 'out/mesa-ref/src/nouveau/nil/copy.rs').read_text()
    ref = copy.split('impl<C: CopyBytes> CopyGOBLines for CopyGOBBlackwell2D2BPP<C>', 1)[1]
    ref = ref.split('struct CopyGOBBlackwell2D1BPP', 1)[0]
    lines = re.findall(r'f\(i \* 0x100 \+ (0x[0-9a-f]+), i \* 32 \+ (\d+), (\d+), 0\);', ref)
    assert len(lines) == 16
    assert 'f(x * 0x40 + y * 0x8, x * 0x8, y, 0);' in copy
    tables = [{}, {}]
    for x in range(8):
        for y in range(8):
            for b in range(8): tables[0][x * 64 + y * 8 + b] = (x * 8 + b, y)
    for column in range(2):
        for offset, x, y in lines:
            for b in range(16):
                tables[1][column * 256 + int(offset, 16) + b] = (column * 32 + int(x) + b, int(y))
    for table in tables:
        assert set(table) == set(range(512))
        assert set(table.values()) == {(x, y) for x in range(64) for y in range(8)}
    return tables


def reference_plan(p, size, pic_size):
    """Unbounded Python sizing, independent of the production planning helper."""
    align = lambda n, a: (n + a - 1) // a * a
    w, h = p['w'], p['h']; pitch = align(w, 64)
    sizes = [size + 16, pic_size, 8,
             align(align(h // 16, 2) * (w // 16) * 64 - 63, 256) * (p['refs'] + 1),
             align((w // 16) * 768, 512), align((w // 16) * 104, 256),
             56, pitch * align(h, 16), pitch * align(h // 2, 16)]
    cursor, job = 4096, []
    for i, size in enumerate(sizes):
        cursor = align(cursor, 1024 if i >= 7 else 256)
        job += [cursor, size]; cursor += size
    return job + [pitch, align(h, 16), align(h // 2, 16), align(cursor, 65536)]


def reference_pixels(p, job, maps, neutral=False):
    w, h = p['w'], p['h']; pitch = job[18]
    linear = bytearray(w * h * 3 // 2)
    tiled = bytearray([0xe7] * (job[15] + job[17]))
    for plane in range(2):
        height, start = (h // 2, w * h) if plane else (h, 0)
        for y in range(height):
            for x in range(w):
                linear[start + y * w + x] = 128 if neutral else (x * 17 + y * 71 + (x // 7) * 23 + plane * 113) & 255
        at = job[15] if plane else 0
        for row in range(0, job[20] if plane else job[19], 16):
            for col in range(0, pitch, 64):
                for gob in range(2):
                    for offset in range(512):
                        x, y = maps[plane][offset]; x += col; y += row + gob * 8
                        if x < w and y < height: tiled[at + offset] = linear[start + y * w + x]
                    at += 512
    return bytes(linear), bytes(tiled)


HARNESS = r'''
#include <stddef.h>
#include <stdbool.h>
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *,const void *,size_t);
extern void *memset(void *,int,size_t);
extern int memcmp(const void *,const void *,size_t);
extern int printf(const char *,...);
extern void abort(void);
void __main(void) {}
#define assert(x) ((x)?(void)0:(printf("harness line %d: %s\n",__LINE__,#x),abort()))
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned char u8;
#define E_INVAL 22
#define E_IO 5
#define E_NOMEM 12
#define E_NOSYS 38
#define CH_NVDEC 0
typedef struct {bool open,submit_failed;u32 pb_at;volatile u32 *sem;volatile u8 *pushbuf;} nv_channel_t;
/* Model the no-scheduler path, not a fabricated completion interrupt. */
static u32 nv_completion_awaken(nv_channel_t *ch,u32 flag){(void)ch;(void)flag;return 0;}
typedef struct {u64 fb;u32 bytes;bool allocated,linear_attempted,tiled_attempted,quarantined;} nvdec_user_buffer_t;
static nvdec_user_buffer_t g_nvdec_user_buffer;
static nv_channel_t channels[1];
static u32 ring[128],sem[1],counts[11],scenario;
static u8 *gpu;
static const u8 *input_ref,*tiled_ref;
static const u32 *packet_ref;
static u32 input_size,packet_words,slice_start,slice_end;
static nvdec_h264_job_layout expected;
static int params[15];
/* counts: heap alloc/free, VRAM alloc/upload, reserve/submit, read/release,
 * cache flush, retired fence, modeled delayed polls. */
static void *kmalloc(u32 bytes){counts[0]++;assert(bytes==expected.total_bytes);return scenario==1?NULL:malloc(bytes);}
static void kfree(void *p){counts[1]++;free(p);}
static bool nvdec_user_allocate(nv_channel_t *ch,u32 bytes){
    counts[2]++;assert(ch==channels && bytes==expected.total_bytes && !gpu);
    if(scenario==2)return false;
    gpu=malloc(bytes);assert(gpu);memset(gpu,0xa6,bytes);
    g_nvdec_user_buffer=(nvdec_user_buffer_t){.fb=0x80000000ull,.bytes=bytes,.allocated=true};return true;
}
static bool nvdec_user_release(nv_channel_t *ch){
    counts[7]++;assert(ch==channels && gpu && !ch->submit_failed && !g_nvdec_user_buffer.quarantined);
    if(scenario==14){g_nvdec_user_buffer.quarantined=true;return false;}
    free(gpu);gpu=NULL;g_nvdec_user_buffer=(nvdec_user_buffer_t){0};return true;
}
static bool nv_vram_object_write(nv_channel_t *ch,u32 object,u64 fb,u32 off,const void *p,u32 bytes){
    counts[3]++;assert(ch==channels && gpu && object==0x004e8000u && fb==0x80000000ull && !off);
    assert(bytes==expected.total_bytes && !counts[5] && !counts[6]);
    if(scenario==3)return false;
    memcpy(gpu,p,bytes);
    assert(!memcmp(gpu+expected.stream_offset,input_ref,input_size));
    static const u8 eos[16]={0,0,1,11,0,0,0,0,0,0,1,11,0,0,0,0};
    assert(!memcmp(gpu+expected.stream_offset+input_size,eos,16));
    const u32 *sl=(const u32 *)(gpu+expected.slice_offset);
    assert(sl[0]==slice_start && sl[1]==slice_end);
    const nvdec_h264_pic_s *pic=(const nvdec_h264_pic_s *)(gpu+expected.picture_offset);
    assert(pic->stream_len==input_size+16 && pic->slice_count==1 && pic->explicitEOSPresentFlag==1);
    assert(!memcmp(pic->eos,eos,16));
    assert(pic->PicWidthInMbs==(u32)params[0]/16 && pic->FrameHeightInMbs==(u32)params[1]/16);
    assert(pic->pitch_luma==expected.pitch && pic->pitch_chroma==expected.pitch);
    assert(pic->luma_bot_offset==(u32)params[0] && pic->chroma_bot_offset==expected.pitch/2);
    assert(pic->HistBufferSize==expected.history_bytes/256 && pic->mbhist_buffer_size==expected.mbhist_bytes);
    assert(pic->tileFormat==1 && !pic->gob_height && pic->frame_mbs_only_flag && pic->ref_pic_flag);
    assert(pic->chroma_format_idc==1 && pic->frame_num==0 && pic->pic_order_cnt_type==params[2]);
    assert(pic->log2_max_frame_num_minus4==params[3] && pic->log2_max_pic_order_cnt_lsb_minus4==params[4]);
    assert(pic->pic_init_qp_minus26==params[5] && pic->chroma_qp_index_offset==params[6]);
    assert(pic->second_chroma_qp_index_offset==params[7] && pic->deblocking_filter_control_present_flag==params[8]);
    assert(pic->direct_8x8_inference_flag==params[9] && pic->pic_order_present_flag==params[10]);
    assert(pic->redundant_pic_cnt_present_flag==params[11]);
    assert(pic->CurrFieldOrderCnt[0]==params[12] && pic->CurrFieldOrderCnt[1]==params[13]);
    for(u32 i=0;i<expected.status_bytes;i++)assert(gpu[expected.status_offset+i]==255);
    /* Every unpopulated scratch/guard/padding byte is initialized, not heap data. */
    for(u32 i=0;i<bytes;i++){
        if((i>=expected.stream_offset && i<expected.stream_offset+input_size+16) ||
           (i>=expected.picture_offset && i<expected.picture_offset+sizeof *pic) ||
           (i>=expected.slice_offset && i<expected.slice_offset+8) ||
           (i>=expected.status_offset && i<expected.status_offset+56))continue;
        assert(gpu[i]==0);
    }
    return true;
}
static bool pb_reserve(nv_channel_t *ch,u32 bytes){
    counts[4]++;assert(ch==channels && bytes>=136 && ch->pb_at+bytes<=sizeof ring);return scenario!=4;
}
static u32 next_completion_signal(nv_channel_t *ch,u32 tag){assert(ch==channels && tag==0x56440000u);return tag|1u;}
static void cache_flush(const void *p,u32 bytes){counts[8]++;assert(p==(const void *)sem && bytes==4 && !sem[0]);}
static bool submit_and_wait(nv_channel_t *ch,u32 start,u32 signal){
    counts[5]++;assert(ch==channels && start==32 && signal==0x56440001u && counts[8]==1);
    assert(ch->pb_at==start+packet_words*4 && !memcmp((const u8 *)ring+start,packet_ref,packet_words*4));
    assert(!counts[6] && !counts[7] && !sem[0]);
    if(scenario==5){ch->submit_failed=true;return false;}
    if(scenario==16){for(u32 i=0;i<3;i++){counts[10]++;assert(!counts[6] && !counts[7] && !sem[0]);}}
    nvdec_frame_status_t status={0};status.mbs_correctly_decoded=(u32)params[0]/16*((u32)params[1]/16);
    status.cycle_count=1995;
    if(scenario==7)status.error_status=0x123;
    if(scenario==8)status.slice_header_error_code=0x456;
    if(scenario==9)status.mbs_in_error=1;
    if(scenario==10)status.mbs_correctly_decoded--;
    if(scenario==11)status.mbs_correctly_decoded++;
    if(scenario!=15)memcpy(gpu+expected.status_offset,&status,sizeof status);
    memcpy(gpu+expected.luma_offset,tiled_ref,expected.luma_bytes+expected.chroma_bytes);
    sem[0]=signal;counts[9]++;return true;
}
static bool nv_vram_object_read(nv_channel_t *ch,u32 object,u64 fb,u32 off,void *p,u32 bytes){
    counts[6]++;assert(ch==channels && object==0x004e8000u && fb==0x80000000ull && gpu);
    assert(counts[9]==1 && sem[0]==0x56440001u && !ch->submit_failed && !g_nvdec_user_buffer.quarantined);
    if(counts[6]==1){assert(off==expected.status_offset && bytes==56);if(scenario==6)return false;}
    else if(counts[6]==2){assert(off==expected.luma_offset && bytes==expected.luma_bytes);if(scenario==12)return false;}
    else {assert(counts[6]==3 && off==expected.chroma_offset && bytes==expected.chroma_bytes);if(scenario==13)return false;}
    assert(off<=expected.total_bytes && bytes<=expected.total_bytes-off);memcpy(p,gpu+off,bytes);return true;
}
__declspec(dllexport) void reset_model(u32 s,u32 state){
    /* A new independent test device. This is not production timeout recovery. */
    free(gpu);gpu=NULL;memset(counts,0,sizeof counts);memset(ring,0xa9,sizeof ring);sem[0]=0xbad;
    g_nvdec_user_buffer=(nvdec_user_buffer_t){.quarantined=state==3};scenario=s;
    channels[0]=(nv_channel_t){.open=state!=1,.submit_failed=state==2,.pb_at=32,.sem=sem,.pushbuf=(u8 *)ring};
}
__declspec(dllexport) void prepare(const u32 *j,const int *p,const u8 *input,u32 n,u32 ss,u32 se,const u8 *tile,const u32 *packet,u32 words){
    memcpy(&expected,j,sizeof expected);memcpy(params,p,sizeof params);input_ref=input;input_size=n;
    slice_start=ss;slice_end=se;tiled_ref=tile;packet_ref=packet;packet_words=words;
}
__declspec(dllexport) u32 counter(u32 i){assert(i<11);return counts[i];}
__declspec(dllexport) u32 quarantine(void){return g_nvdec_user_buffer.quarantined;}
__declspec(dllexport) u32 pic_size(void){return sizeof(nvdec_h264_pic_s);}
'''


def main():
    source = (ROOT / 'kernel/nv_chan.c').read_text()
    headers = ('kernel/nvdec_h264_context.h', 'kernel/nvdec_h264_job.h',
               'kernel/nv_video_layout.h', 'include/kestrel/video.h')
    code = ''.join('#include "' + (ROOT / p).as_posix() + '"\n' for p in headers)
    for name in ('VA_NVDEC_USER', 'VA_NVDEC_USER_TILED', 'VA_SEM', 'H_NVDEC_USER_VRAM',
                 'SUBCH_NVDEC', 'NVCFB0_VIDEO_DECODER', 'NV906F_SET_OBJECT_ENGINE_SW',
                 'VIDEO_COMPLETION_AWAKEN'):
        code += re.search(r'^#define ' + name + r'\s+[^\n]+', source, re.M)[0] + '\n'
    code += HARNESS + '\n' + function(source, 'pb_method') + '\n' + function(source, 'pb_data')
    code += '\n' + function(source, 'nv_nvdec_decode_idr')
    code += '\n__declspec(dllexport) int decode(const u8 *p,u32 n,u8 *o,u32 c,kvideo_request_t *r){return nv_nvdec_decode_idr(p,n,o,c,r);}\n'
    clang = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
    maps = reference_maps(); count = compared = 0
    with tempfile.TemporaryDirectory(prefix='kestrel-nvdec-user-') as tmp, ExitStack() as cleanup:
        c, obj, dll = [Path(tmp) / ('test.' + ext) for ext in ('c', 'obj', 'dll')]; c.write_text(code)
        # NVIDIA picture ABI uses Linux/SysV bitfield packing, not Microsoft's.
        subprocess.run([clang, '--target=x86_64-w64-windows-gnu', '-mno-ms-bitfields',
                        '-ffreestanding', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-sign-compare', '-fsanitize=undefined', '-fsanitize-trap=all',
                        '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([clang, '-shared', str(obj), '-o', str(dll)], check=True)
        lib = C.CDLL(str(dll)); cleanup.callback(_ctypes.FreeLibrary, lib._handle)
        lib.prepare.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_uint, C.c_uint, C.c_uint,
                                C.c_void_p, C.c_void_p, C.c_uint]
        lib.decode.argtypes = [C.c_void_p, C.c_uint, C.c_void_p, C.c_uint, C.POINTER(Request)]
        fixture = (ROOT / 'tools/h264_iframe_test.h').read_text().split('h264_iframe_test[]', 1)[1].split('{', 1)[1].split('}', 1)[0]
        fixture = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', fixture))

        def setup(data, p, neutral=False, scenario=0, state=0):
            job = reference_plan(p, len(data), lib.pic_size())
            linear, tiled = reference_pixels(p, job, maps, neutral)
            # Fixed NVIDIA method ABI/order, independently encoded. In particular
            # picture/scratch use pitch VA, output uses kind-6 tiled alias VA.
            linear_va, tiled_va = 0xa00000000, 0xa80000000
            methods = [(0, 0x1fcfb0), (0x200, 3), (0x400, 0x2053),
                (0x404, (linear_va + job[2]) >> 8), (0x408, (linear_va + job[0]) >> 8),
                (0x40c, 0), (0x410, (linear_va + job[4]) >> 8),
                (0x414, (linear_va + job[6]) >> 8), (0x418, (linear_va + job[8]) >> 8),
                (0x424, (linear_va + job[12]) >> 8), (0x430, (tiled_va + job[14]) >> 8),
                (0x474, (tiled_va + job[16]) >> 8), (0x500, (linear_va + job[10]) >> 8), (0x300, 0)]
            packet = [word for method, value in methods for word in (0x20018000 + method // 4, value)]
            packet += [0x20038090, 2, 0x00200000, 0x56440001, 0x200180c1, 0]
            fields = [p['w'], p['h'], p['poc'], p['fbits'] - 4, p['pbits'] - 4 if p['poc'] == 0 else 0,
                      p['qp'], p['chroma'], p['chroma'] if p['second_chroma'] is None else p['second_chroma'],
                      p['deblock'], p['direct'], p['bottom'], p['redundant'], p['poc_lsb'],
                      p['poc_lsb'] + p['delta_bottom'], 0]
            buffers = (C.create_string_buffer(data), C.create_string_buffer(tiled),
                       (C.c_uint * len(job))(*job), (C.c_int * len(fields))(*fields),
                       (C.c_uint * len(packet))(*packet))
            ib, tb, jb, pb, cmds = buffers
            lib.reset_model(scenario, state)
            starts = list(re.finditer(b'\x00{2,3}\x01', data))
            slices = [(m.start(), (starts[i + 1].start() if i + 1 < len(starts) else len(data)))
                      for i, m in enumerate(starts) if data[m.end()] & 31 == 5]
            assert len(slices) == 1
            ss, se = slices[0]
            while data[se - 1] == 0: se -= 1
            lib.prepare(jb, pb, ib, len(data), ss, se, tb, cmds, len(packet))
            return buffers, linear

        def run(data, p, scenario=0, state=0, operation=1, capacity=None, null_output=False, neutral=False):
            nonlocal count, compared
            keep, linear = setup(data, p, neutral, scenario, state); ib = keep[0]
            guard = 64; out = C.create_string_buffer(bytes([0xa5]) * (len(linear) + 2 * guard))
            before = bytes(out); incoming = bytes(ib)
            r = Request(); C.memset(C.byref(r), 0xa5, C.sizeof(r)); r.version = 1; r.operation = operation
            count += 1
            rc = lib.decode(ib, len(data), None if null_output else C.addressof(out) + guard,
                            len(linear) if capacity is None else capacity, C.byref(r))
            assert bytes(ib) == incoming
            assert bytes(out)[:guard] == before[:guard] and bytes(out)[guard + len(linear):] == before[guard + len(linear):]
            stats = [lib.counter(i) for i in range(11)]
            assert (r.coded_width, r.coded_height, r.pitch, r.required_bytes) == (p['w'], p['h'], p['w'], len(linear))
            crop = tuple(2 * x for x in p['crop'])
            assert (r.crop_left, r.crop_right, r.crop_top, r.crop_bottom) == crop
            assert (r.display_width, r.display_height) == (p['w'] - crop[0] - crop[1], p['h'] - crop[2] - crop[3])
            assert r.reserved == 0 and r.parse_status == 0
            if operation == 0:
                assert rc == 0 and r.phase == 10 and not r.written_bytes and not any(stats) and bytes(out) == before
            elif scenario in (0, 16) and state == 0 and not null_output and (capacity is None or capacity >= len(linear)):
                assert rc == 0 and r.phase == 10 and r.written_bytes == len(linear)
                assert bytes(out)[guard:guard + len(linear)] == linear
                assert stats == [1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 3 if scenario == 16 else 0]
                assert r.decoded_mbs == p['w'] // 16 * (p['h'] // 16)
                compared += len(linear)
            else:
                assert rc < 0 and r.written_bytes == 0
                # Release failure can affect the private kernel shadow; SYS_GPU
                # must not publish that shadow. It is tested separately.
                if scenario != 14: assert bytes(out) == before
            return rc, r, stats, keep, out

        _, base, _, _ = stream(w=64, h=64, deblock=0)
        run(fixture, base, neutral=True)
        # Mixed start-code lengths, auxiliary NALs and trailing zeros must not
        # turn slice_end into the full upload length or omit the original prefix.
        wrapped = b'\0\0\0\1\x09\xf0' + fixture.replace(b'\0\0\0\1\x65', b'\0\0\1\x65')
        wrapped += b'\0\0\0\0\1\x0a\x80\0\0'
        run(wrapped, base, neutral=True)
        rng = random.Random(0xdec064)
        cases = [dict(w=48, h=64), dict(w=256, h=256), dict(w=1920, h=1088, crop=(0, 0, 0, 4)),
                 dict(w=320, h=240, crop=(1, 2, 3, 4), qp=-12, chroma=-8, second_chroma=7,
                      direct=1, fbits=9, pbits=12, bottom=1, poc_lsb=351, delta_bottom=-6, redundant=1)]
        cases += [dict(w=rng.randrange(3, 34) * 16, h=rng.randrange(4, 24) * 16,
                       poc=2 if i % 3 == 0 else 0, refs=i % 17, qp=i % 26 - 13,
                       chroma=i % 13 - 6, deblock=i % 2) for i in range(28)]
        for values in cases:
            data, p, _, _ = stream(**values); run(data, p, scenario=16 if values == cases[0] else 0)
        data, p, _, _ = stream(w=80, h=80, crop=(1, 2, 3, 4))
        # INSPECT is CPU header parsing only, even with a missing/failed GPU.
        for state in range(4): run(data, p, state=state, operation=0, null_output=True)
        for state, error in [(1, -38), (2, -5), (3, -5)]:
            rc, r, stats, *_ = run(data, p, state=state)
            assert rc == error and r.phase == 2 and not any(stats)
        for capacity in (0, 1, p['w'] * p['h'] * 3 // 2 - 1):
            rc, r, stats, *_ = run(data, p, capacity=capacity)
            assert rc == -22 and r.phase == 2 and not any(stats)
        rc, r, stats, *_ = run(data, p, null_output=True)
        assert rc == -22 and not any(stats)
        expected = {
            1: (-12, 3, [1,0,0,0,0,0,0,0,0,0,0]),
            2: (-5, 3, [1,1,1,0,0,0,0,0,0,0,0]),
            3: (-5, 4, [1,1,1,1,0,0,0,1,0,0,0]),
            4: (-5, 5, [1,1,1,1,1,0,0,1,0,0,0]),
            5: (-5, 5, [1,1,1,1,1,1,0,0,1,0,0]),
            6: (-5, 6, [1,1,1,1,1,1,1,1,1,1,0]),
            12: (-5, 7, [1,1,1,1,1,1,2,1,1,1,0]),
            13: (-5, 7, [1,1,1,1,1,1,3,1,1,1,0]),
            14: (-5, 9, [1,1,1,1,1,1,3,1,1,1,0]),
        }
        for s in (7, 8, 9, 10, 11, 15): expected[s] = (-5, 6, [1,1,1,1,1,1,1,1,1,1,0])
        for scenario, (error, phase, counters) in expected.items():
            rc, r, stats, keep, out = run(data, p, scenario=scenario)
            assert (rc, r.phase, stats) == (error, phase, counters), (scenario, rc, r.phase, stats)
            if scenario == 7: assert r.firmware_error == 0x123
            if scenario == 8: assert r.slice_error == 0x456
            if scenario == 9: assert r.error_mbs == 1
            if scenario in (5, 14):
                assert lib.quarantine() == 1
                # A second call cannot reuse or release a potentially live job.
                count += 1; old = bytes(out)
                assert lib.decode(keep[0], len(data), out, len(out), C.byref(r)) == -5
                assert r.written_bytes == 0 and bytes(out) == old
                assert [lib.counter(i) for i in range(11)] == stats

        # Invalid/truncated/unsupported input and geometry never allocate or touch GPU.
        invalid = [b'', b'\0\0\1', b'not annexb', data[:5], data[:data.index(b'\0\0\0\1\x65')]]
        invalid += [stream(**v)[0] for v in (dict(cabac=1), dict(profile=100), dict(frame_only=0),
                                            dict(w=32, h=64), dict(w=64, h=48), dict(slice_type=0))]
        invalid.append(data + data[data.index(b'\0\0\0\1\x65'):])
        for bad in invalid:
            lib.reset_model(0, 0); ib = C.create_string_buffer(bad); out = C.create_string_buffer(b'unchanged')
            r = Request(version=1, operation=1, written_bytes=123, firmware_error=123)
            count += 1
            assert lib.decode(ib, len(bad), out, len(out), C.byref(r)) == -22
            assert r.parse_status and not r.written_bytes and not r.firmware_error and bytes(out) == b'unchanged\0'
            assert not any(lib.counter(i) for i in range(11))
        for length in (0, 123, 16 * 1024 * 1024 + 1):
            lib.reset_model(0, 0); r = Request(version=1, operation=1); count += 1
            assert lib.decode(None, length, None, 0, C.byref(r)) == -22
            assert not r.written_bytes and not any(lib.counter(i) for i in range(11))
        for version, op in ((0, 1), (2, 1), (1, 2), (1, 0xffffffff)):
            lib.reset_model(0, 0); r = Request(version=version, operation=op); count += 1
            assert lib.decode(None, 0, None, 0, C.byref(r)) == -22
            assert not any(lib.counter(i) for i in range(11))
        count += 1; assert lib.decode(None, 0, None, 0, None) == -22
        lib.reset_model(0, 0)
    print(f'NVDEC actual user decode control flow PASS: {count} cases, {compared:,} independently modeled pixel bytes; no native GPU claim')


if __name__ == '__main__': main()
