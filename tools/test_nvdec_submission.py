#!/usr/bin/env python3
"""Run production NVDEC submission and readback gates with modeled firmware.

Checks the NVIDIA status ABI independently. Does not prove hardware execution.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from test_gpu_stable_candidate import function

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "kernel/nv_chan.c").read_text()


def main():
    code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern int puts(const char *);
extern int printf(const char *, ...);
extern void abort(void);
void __main(void) {}
#define assert(x) ((x) ? (void)0 : (printf("scenario %d line %d: %s\n", scenario, __LINE__, #x), abort()))
typedef uint8_t u8; typedef uint16_t u16;
typedef uint32_t u32; typedef uint64_t u64;
#define kinfo(...) ((void)0)
#define kerr(...) ((void)0)
#define kwarn(...) ((void)0)
typedef struct {
    bool open, submit_failed;
    u32 pb_at;
    volatile u32 *sem;
    volatile u8 *pushbuf;
} nv_channel_t;
/* No scheduler/IRQ route exists in this model; preserve the boot release word. */
static u32 nv_completion_awaken(nv_channel_t *ch,u32 flag) { (void)ch;(void)flag;return 0; }
#define CH_NVDEC 0
static int scenario;
static u32 sem, ring[0x20000];
static nv_channel_t channels[1];
static u64 g_nvdec_vram_fb = 0x2000000;
static u8 memory[0x20000];
static unsigned submits, reads, writes;
static bool ensure_nvdec_vram(nv_channel_t *ch) { (void)ch; return scenario != 3; }
static void cache_flush(const volatile void *p, u32 n) { (void)p; (void)n; }
static bool pb_reserve(nv_channel_t *ch, u32 n) {
    assert(ch->pb_at >= 0x10000 && ch->pb_at + n <= sizeof ring);
    return scenario != 5;
}
static u32 next_completion_signal(nv_channel_t *ch, u32 tag) {
    (void)ch; return tag | 1;
}
static bool nv_vram_object_write(nv_channel_t *ch, u32 obj, u64 fb,
                                 u64 off, const void *p, u64 n) {
    (void)ch; (void)obj; (void)fb;
    assert(off+n <= sizeof memory && off == 0x1000 && n == 0xb000);
    assert(submits == 0); writes++;
    memcpy(memory+off, p, n);
    for (unsigned i=0x6000; i<0x7000; i++) assert(memory[i] == 0xff);
    return scenario != 4;
}
static bool nv_vram_object_read(nv_channel_t *ch, u32 obj, u64 fb,
                                u64 off, void *p, u64 n) {
    (void)ch; (void)obj; (void)fb;
    assert(submits == 1 && scenario != 6);
    assert(off+n <= sizeof memory && !(off & 3) && !(n & 3));
    reads++;
    if ((scenario == 16 && off == 0x8000) ||
        (scenario == 17 && off == 0xa000) ||
        (scenario == 18 && off == 0x6000)) return false;
    if (off == 0x8000) assert(n == 4096);
    else if (off == 0xa000) assert(n == 2048);
    else { assert(off == 0x6000 && n == 56); }
    memcpy(p, memory+off, n); return true;
}
'''
    for path in ["kernel/nvdec_drv_h264.h", "kernel/nvdec_h264_context.h", "tools/h264_iframe_test.h"]:
        code += '\n#include "' + (ROOT / path).as_posix() + '"\n'
    official = (ROOT / "refs/open-gpu-doc/classes/video/nvdec_drv.h").read_text()
    start = official.index("typedef struct _nvdec_status_hevc_s")
    end = official.index("} nvdec_status_s;", start) + len("} nvdec_status_s;")
    code += official[start:end] + "\n"
    code += '_Static_assert(sizeof(nvdec_frame_status_t) == sizeof(nvdec_status_s), "official status size");\n'
    for field in ["mbs_correctly_decoded", "mbs_in_error", "cycle_count",
                  "error_status", "slice_header_error_code"]:
        code += f'_Static_assert(offsetof(nvdec_frame_status_t, {field}) == offsetof(nvdec_status_s, {field}), "official {field}");\n'
    for name in ["SUBCH_NVDEC", "NV906F_SET_OBJECT_ENGINE_SW", "VA_NVDEC",
                 "VA_NVDEC_TILED", "H_NVDEC_VRAM", "NVDEC_VRAM_BYTES", "VA_SEM",
                 "NVCFB0_VIDEO_DECODER", "VIDEO_COMPLETION_AWAKEN"]:
        code += re.search(r"^#define\s+" + name + r"\s+[^\n]*", SRC, re.M)[0] + "\n"
    code += function(SRC, "pb_method") + "\n" + function(SRC, "pb_data")
    code += r'''
static bool submit_and_wait(nv_channel_t *ch, u32 start, u32 signal) {
    submits++;
    bool bound = false, executed = false, semaphore = false, released = false;
    for (u32 at=start/4; at<ch->pb_at/4;) {
        u32 header=ring[at++], count=(header>>16)&0x1fff;
        u32 method=(header&0x1fff)*4;
        assert((header>>29)==1 && ((header>>13)&7)==4);
        assert(count && at+count <= ch->pb_at/4);
        if (method == 0) {
            assert(!bound && count == 1);
            assert(ring[at] == (0xcfb0 | (0x1fu<<16))); bound=true;
        } else assert(bound);
        if (method == 0x424) assert(ring[at] == (u32)((VA_NVDEC+0x6000)>>8));
        if (method == 0x430) assert(ring[at] == (u32)((VA_NVDEC_TILED+0x8000)>>8));
        if (method == 0x474) assert(ring[at] == (u32)((VA_NVDEC_TILED+0xa000)>>8));
        if (method == 0x300) {
            assert(!executed && !released && count == 1 && !ring[at]); executed=true;
        }
        if (method == 0x240) {
            assert(executed && !semaphore && count == 3); semaphore=true;
            assert(ring[at] == (u32)(VA_SEM>>32) && ring[at+1] == (u32)VA_SEM && ring[at+2] == signal);
        }
        if (method == 0x304) {
            assert(executed && semaphore && !released && count == 1 && !ring[at]); released=true;
        }
        at += count;
    }
    assert(executed && released && writes == 1);
    if (scenario == 6) { ch->submit_failed=true; return false; }
    memset(memory+0x8000, 128, 4096);
    memset(memory+0xa000, 128, 2048);
    if (scenario == 7) return true; /* poisoned/unwritten status must fail */
    nvdec_status_s st = {0}; /* produce the official, not our mirrored, ABI */
    st.mbs_correctly_decoded = 16;
    st.cycle_count = scenario == 19 ? 0 : 1234;
    if (scenario == 8) st.mbs_correctly_decoded = 0;
    if (scenario == 9) st.mbs_correctly_decoded = 15;
    if (scenario == 10) st.mbs_correctly_decoded = 17;
    if (scenario == 11) st.mbs_in_error = 1;
    if (scenario == 12) st.error_status = 1;
    if (scenario == 13) st.slice_header_error_code = 1;
    if (scenario == 14) memory[0x8fff] = 0;
    if (scenario == 15) memory[0xa7ff] = 0;
    memcpy(memory+0x6000, &st, sizeof st);
    return true;
}
'''
    code += r'''
/* Mutable harness-only copy; production fixture remains const. */
static unsigned char active_fixture[H264_TEST_LEN];
static void reset_fixture(void) { memcpy(active_fixture,h264_iframe_test,H264_TEST_LEN); }
#define h264_iframe_test active_fixture
'''
    code += '\n#include "' + (ROOT / 'tools/fixtures/nvdec_64x64_legacy_context.h').as_posix() + '"\n'
    code += function(SRC, "nv_nvdec_selftest_hw")
    code += r'''
int main(void) {
    nvdec_h264_pic_s previous, parsed;
    kh264_decode_desc desc;
    reset_fixture();
    nvdec_fill_pic(&previous,H264_TEST_LEN+16,64,64,0xc00,0x200);
    assert(nvdec_h264_parse_pic(&parsed,&desc,h264_iframe_test,H264_TEST_LEN,
                               64,64,0xc00,0x200)==KH264_OK);
    /* Preserve every hardware-visible byte of the successful native fixture. */
    assert(!memcmp(&previous,&parsed,sizeof parsed));
    assert(desc.slice_start==H264_TEST_SLICE_OFFSET && desc.slice_end==H264_TEST_LEN);
    for (scenario=0; scenario<23; scenario++) {
        reset_fixture();
        if(scenario==20)active_fixture[4]|=0x80; /* forbidden_zero_bit */
        if(scenario==21)active_fixture[5]=100; /* unsupported high profile */
        if(scenario==22)active_fixture[16]|=0x20; /* PPS entropy_coding_mode: CABAC */
        memset(ring, 0, sizeof ring); memset(memory, 0, sizeof memory); sem=0;
        channels[0]=(nv_channel_t){.open=scenario!=1, .submit_failed=scenario==2,
                                  .pb_at=0x10000, .sem=&sem, .pushbuf=(u8 *)ring};
        submits=reads=writes=0;
        int rc=nv_nvdec_selftest_hw();
        if(writes)assert(!memcmp(memory+0x3000,&parsed,sizeof parsed));
        assert((rc==0) == (scenario==0 || scenario==19));
        if (scenario>=1 && scenario<=6) assert(reads==0);
        if ((scenario>=1 && scenario<=3) || scenario>=20) assert(writes==0 && submits==0 && reads==0);
        if (scenario==4 || scenario==5) assert(writes==1 && submits==0);
        if (scenario==6) {
            assert(channels[0].submit_failed);
            assert(nv_nvdec_selftest_hw()==1 && submits==1 && writes==1 && reads==0);
        }
        if (scenario==16) assert(reads==1);
        else if (scenario==17) assert(reads==2);
        else if (scenario==0 || (scenario>=7 && scenario<20)) assert(reads==3);
    }
    puts("NVDEC production submission: official status ABI, byte-identical parsed fixture, malformed headers rejected before upload, full NV12, launch/fence ordering and failure gates PASS (23 cases; modeled hardware)");
    return 0;
}
'''
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-nvdec-test-") as tmp:
        c, obj, exe = (Path(tmp) / n for n in ("test.c", "test.obj", "test.exe"))
        c.write_text(code)
        subprocess.run([clang, "--target=x86_64-w64-windows-gnu", "-mno-ms-bitfields",
                        "-ffreestanding", "-std=c11", "-O1", "-Wall", "-Wextra",
                        "-c", str(c), "-o", str(obj)], check=True)
        subprocess.run([clang, str(obj), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
