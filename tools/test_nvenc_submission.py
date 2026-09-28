#!/usr/bin/env python3
"""Execute the production NVENC smoke test with modeled firmware completion.

Checks command order and failure gates, not physical Falcon execution.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile
from test_gpu_stable_candidate import function

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "kernel/nv_chan.c").read_text()


def main(compile_only=False):
    preamble = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern int puts(const char *);
extern int printf(const char *, ...);
extern int fflush(void *);
extern void abort(void);
void __main(void) {} /* GNU C constructor hook: this harness has no constructors. */
#define assert(x) ((x) ? (void)0 : (printf("assertion line %d: %s\n", __LINE__, #x), fflush(NULL), abort()))
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
static void nvenc_capture_rc_journal(nv_channel_t *ch) { (void)ch; }
/* Independent readback diagnostic has its own compile-only scaffold; it must
 * not change this model's completion, output or channel-quarantine state. */
static void nvenc_capture_unretired_status(nv_channel_t *ch,u64 offset) { (void)ch;(void)offset; }
#define CH_NVENC 0
static u32 sem, ring[1024];
static nv_channel_t channels[1];
static u64 g_nvenc_vram_fb = 0x2000000;
static u8 memory[0xc0000];
static unsigned submits, reads, writes, output_read_bytes, sequence;
static u32 reserved_end;
static bool g_nvenc_transport_ready;
static int scenario;
static bool ensure_nvenc_vram(nv_channel_t *ch) { (void)ch; return true; }
static void cache_flush(const volatile void *p, u32 n) { (void)p; (void)n; }
static bool pb_reserve(nv_channel_t *ch, u32 n) {
    assert(ch->pb_at + n <= sizeof ring);
    reserved_end = ch->pb_at + n;
    return scenario != 10 && !(scenario == 33 && submits == 1);
}
static u32 next_completion_signal(nv_channel_t *ch, u32 tag) {
    (void)ch; return tag | ++sequence;
}
static bool nv_vram_object_write(nv_channel_t *ch, u32 obj, u64 fb,
                                 u64 off, const void *p, u64 n) {
    (void)ch; (void)obj; (void)fb;
    assert(off+n <= sizeof memory); writes++;
    memcpy(memory+off, p, n); return scenario != 9;
}
static bool nv_vram_object_read(nv_channel_t *ch, u32 obj, u64 fb,
                                u64 off, void *p, u64 n) {
    (void)ch; (void)obj; (void)fb;
    assert(submits == 2 && scenario != 1);
    assert(off+n <= sizeof memory && (off & 3) == 0 && (n & 3) == 0);
    reads++;
    if(off==0xb000)output_read_bytes=(unsigned)n;
    if (scenario == 11 || (scenario == 12 && off == 0xb000)) return false;
    memcpy(p, memory+off, n); return true;
}
'''
    preamble += '\n#include "' + (ROOT / "kernel/nvenc_drv_h264.h").as_posix() + '"\n'
    preamble += '\n#include "' + (ROOT / "kernel/nvenc_h264_policy.h").as_posix() + '"\n'
    preamble += '\n#include "' + (ROOT / "kernel/nvenc_h264_buffers.h").as_posix() + '"\n'
    preamble += '\n#include "' + (ROOT / "kernel/nv_video_layout.h").as_posix() + '"\n'
    preamble += '\n#include "' + (ROOT / "include/kestrel/video.h").as_posix() + '"\n'
    preamble += 'static nvenc_h264_test_output_s g_nvenc_output;\n'
    preamble += 'static nvenc_cfb7_h264_drv_pic_setup_s g_nvenc_output_context;\n'
    names = ["SUBCH_NVENC", "NV906F_SET_OBJECT_ENGINE_SW", "VA_NVENC",
             "VA_NVENC_TILED", "H_NVENC_VRAM", "NVENC_VRAM_BYTES", "NV_VIDEO_PTE_KIND", "VA_SEM",
             "NVCFB7_VIDEO_ENCODER", "VIDEO_COMPLETION_AWAKEN"]
    names += re.findall(r"^#define (NVCFB7_(?!VIDEO_ENCODER)\w+)\s", SRC, re.M)
    for name in dict.fromkeys(names):
        preamble += re.search(r"^#define\s+" + name + r"\s+[^\n]*", SRC, re.M)[0] + "\n"
    preamble += function(SRC, "pb_method") + "\n" + function(SRC, "pb_data")
    preamble += "\n" + function(SRC, "nvenc_address")
    preamble += r'''
static u32 wire32(const u8 *p) {
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}
static bool submit_and_wait(nv_channel_t *ch, u32 start, u32 signal) {
    assert(ch->pb_at <= reserved_end);
    submits++;
    if(submits==1) {
        /* Independent readiness stream: class, H.264 application, semaphore
         * address/payload, then flushing release. No picture or EXECUTE. */
        const u32 expected[]={0x20018000u,0x001fcfb7u,0x20018080u,1u,
            0x20038090u,(u32)(VA_SEM>>32),(u32)VA_SEM,0x45430001u,
            0x200180c1u,0u};
        assert(signal==0x45430001u && ch->pb_at-start==sizeof expected);
        assert(!memcmp(ring+start/4,expected,sizeof expected));
        assert(!writes && !reads && !g_nvenc_transport_ready);
        if(scenario==32){ch->submit_failed=true;return false;}
        return true;
    }
    assert(submits==2 && signal==0x454e0002u && g_nvenc_transport_ready);
    assert(writes==1 && !reads); /* Complete initialization precedes EXECUTE. */
    bool executed = false, released = false;
    unsigned executions = 0, releases = 0, controls = 0, address_packets = 0;
    static u64 addresses[0x800/4];memset(addresses,0,sizeof addresses);
    for (u32 at = start/4; at < ch->pb_at/4;) {
        u32 header = ring[at++], count = (header >> 16) & 0x1fff;
        u32 method = (header & 0xfff) * 4;
        bool address_method = method>=0x70c && method<0x800;
        assert((header >> 29) == (address_method ? 3u : 1u));
        assert(((header >> 13) & 7) == 4);
        assert(count && at+count <= ch->pb_at/4);
        if (method == 0) assert(ring[at] == (0xcfb7 | (0x1fu<<16)));
        if (method == 0x700) {
            /* Independent NVIDIA emitter result for our H264/CONSTQP test:
             * force picture + bound coloc output + enabled GPTIMER. Bit 4
             * (TOPLEVEL) is absent from this emitter, unlike codec 6. */
            assert(!executed && count == 1 && ring[at] == 0x1303);
            controls++;
        }
        if (method == 0x300) { assert(controls == 1 && !released && count == 1 && !ring[at]); executed = true; executions++; }
        if (method == 0x240) {
            assert(executed && count == 3);
            assert(ring[at] == (u32)(VA_SEM>>32) && ring[at+1] == (u32)VA_SEM && ring[at+2] == signal);
        }
        if (method == 0x304) { assert(executed && count == 1 && !ring[at]); released = true; releases++; }
        if(address_method){
            /* Independent Windows + Linux backend rule: non-incrementing
             * low address[39:8], high address[63:40], even when high is zero. */
            assert(!executed && count==2);
            addresses[method/4]=((u64)ring[at]<<8)|((u64)ring[at+1]<<40);
            address_packets++;
        }
        at += count;
    }
    assert(executions == 1 && releases == 1);
    assert(address_packets == 13 && ch->pb_at-start == 220);
    const nvenc_cfb7_h264_drv_pic_setup_s *cfg = (const void *)(memory+0x1000);
    assert(cfg->magic == 0xcfb70006 && cfg->input_cfg.frame_width_minus1 == 255);
    assert(cfg->input_cfg.frame_height_minus1 == 255 && cfg->pic_control.pic_type == 3);
    assert(cfg->pic_control.bitstream_buf_size == 0x40000);
    assert(cfg->rate_control.two_pass_rc == 1);
    for(unsigned i=0;i<3;i++)assert(cfg->rate_control.rhopbi[i]==256);
    assert(cfg->rate_control.maxQPD==6 && cfg->rate_control.baseQPD==3);
    assert(cfg->rate_control.iSizeRatioX==1 && cfg->rate_control.iSizeRatioY==1);
    assert(cfg->pic_control.act_stat_offset == 512);
    assert(addresses[0x70c/4] == VA_NVENC+0xb7000);
    for(unsigned i=0;i<8192;i++)assert(!memory[0xb7000+i]);
    assert(addresses[0x724/4] == VA_NVENC+0xb9000);
    assert(!memcmp(memory+0xb9000,memory+0xb9100,256));
    for(unsigned i=512;i<0x3000;i++)assert(!memory[0xb9000+i]);
    assert(cfg->pic_control.bitstream_buf_size > 256u*256u*3u/2u);
    /* Independent byte offsets established by the 595 binary converter. Do
     * not read these through the same struct that constructed the upload. */
    const u8 *wire = memory+0x1000;
    assert(wire[0xba] == 1); /* actual Linux producer and Windows capture */
    /* Independent successful CFB7 H.264 picture byte. The outer method
     * 0x700 remains 0x1303 above; its codec 3 is NOT this field's enum. */
    assert((wire[0x1ae]&7u)==0u && cfg->pic_control.codec==0);
    /* NVIDIA's full-frame path leaves the strip ID/count/start/rows zero. */
    for(unsigned i=0x1c2;i<0x1c8;i++)assert(wire[i]==0);
    assert(!cfg->pic_control.new_subframe);
    /* 595 sequence producer multiplies fps by the binary double 256.0;
     * picture producer copies it to 0x6784 - 0x66fc = 0x88. Not 16.16. */
    assert(wire32(wire+0x88)==7680u); /* 30 fps in 23.8 fixed point */
    assert(wire32(wire+0x19c)==6144u && cfg->pic_control.hist_buf_size==6144u);
    /* Producer offsets 0x67c4/0x67cc minus picture base 0x66fc. Missing
     * reference is byte 0xfe (builder's -2), NOT zero or generic -1. */
    for(unsigned i=0xc8;i<0xd8;i++)assert(wire[i]==0xfe);
    for(unsigned desc=4;desc<=68;desc+=32){
        assert(wire[desc]==255&&wire[desc+1]==0&&wire[desc+2]==255&&wire[desc+3]==0);
        assert(wire[desc+4]==0&&wire[desc+5]==1&&wire[desc+6]==0&&wire[desc+7]==1);
        /* NVIDIA producer's byte, independent of Kestrel bitfield layout. */
        assert(wire[desc+28]==2&&wire[desc+29]==0&&wire[desc+30]==0&&wire[desc+31]==0);
    }
    assert(wire[0x200]==0x04 && wire[0x201]==0x17 &&
           wire[0x202]==0x03 && wire[0x203]==0x80);
    for(unsigned i=0x1dc;i<0x200;i++)assert(wire[i]==0);
    for(unsigned i=0x204;i<0x208;i++)assert(wire[i]==0);
    assert(wire32(wire+0x208)==0x999u);
    for(unsigned i=0x20c;i<0x300;i++)assert(wire[i]==0);
    assert(wire[0x300] || wire[0x301]); /* slice record still begins after full picture */
    /* Relative auxiliary offsets: 595 producer's this+0x6888 etc minus
     * this+0x66fc picture base. Sizes checked against the actual upload. */
    const unsigned fields[]={0x18c,0x190,0x194,0x198,0x1bc};
    const unsigned offsets[]={0x300,0x400,0x500,0x600,0x700};
    const unsigned sizes[]={128,192,128,192,452};
    for(unsigned i=0x188;i<0x18c;i++)assert(wire[i]==0); // one of each control
    assert(addresses[0x710/4]==VA_NVENC+0x1000);
    assert(addresses[0x718/4]==VA_NVENC+0x3000);
    /* Output arrays previously all pointed at offset zero, aliasing the
     * picture status header. Match the owned capture's first two offsets;
     * reserve worst-case per-MB storage independently of enable flags. */
    assert(cfg->pic_control.slice_stat_offset==0x100);
    assert(cfg->pic_control.mpec_stat_offset==0x1100);
    assert(cfg->pic_control.stats_fifo_offset==0x5100);
    assert(cfg->pic_control.aq_stat_offset==0x5900);
    const unsigned stat_offsets[]={0,0x100,0x1100,0x5100,0x5900,0x8000};
    const unsigned stat_sizes[]={128,256*16,256*64,256*8,256*16};
    for(unsigned i=0;i<5;i++)assert(stat_offsets[i]+stat_sizes[i]<=stat_offsets[i+1]);
    assert(addresses[0x718/4]+0x8000<=addresses[0x71c/4]);
    for(unsigned i=128;i<0x8000;i++)assert(!memory[0x3000+i]);
    /* Model the documented output writes and prove none can clobber the
     * picture completion header or first bitstream byte. */
    for(unsigned i=1;i<5;i++)memset(memory+0x3000+stat_offsets[i],0x3c,stat_sizes[i]);
    for(unsigned i=0;i<128;i++)assert(memory[0x3000+i]==0xa5);
    assert(memory[0xb000]==0);
    for(unsigned i=0;i<5;i++) {
        unsigned off=wire32(wire+fields[i]);assert(off==offsets[i] && !(off&63));
        assert(off>=0x300 && off+sizes[i]<=0x2000);
        if(i<4)assert(off+sizes[i]<=offsets[i+1]);
    }
    /* Fixture semantics, independently decoded as little-endian words:
     * 256 MBs, QP26, forced intra, QP range 0..51, all four boundaries.
     * No reference commands, ROI, header suppression or nonzero aux indices. */
    for(unsigned off=0;off<128;off+=4) {
        u32 expected=off==0?0x00d00100u:off==8?0x01330000u:off==16?15u:0u;
        assert(wire32(wire+offsets[0]+off)==expected);
    }
    /* The five ME hint types must each occur once even in this intra fixture.
     * NVIDIA writes 0x4688 at ME+0x9a: const, spatial, temporal, coloc, external.
     * Keep the other minimal ME controls zero, not an invented inter preset. */
    unsigned hint_order=wire[offsets[1]+0x9a] | (unsigned)wire[offsets[1]+0x9b]<<8;
    assert(hint_order==0x4688u);
    unsigned hint_mask=0;
    for(unsigned i=0;i<5;i++) {
        unsigned type=(hint_order>>(3*i))&7;
        assert(type<5 && !(hint_mask&(1u<<type)));hint_mask|=1u<<type;
    }
    assert(hint_mask==31);
    for(unsigned i=0;i<192;i++) {
        unsigned expected=i==0x9a?0x88:i==0x9b?0x46:0;
        assert(wire[offsets[1]+i]==expected);
    }
    /* Boot supplies the same normal-encode quant defaults as application input. */
    nvenc_h264_quant_control_s expected_quant;
    nvenc_h264_quant_defaults(&expected_quant);
    assert(!memcmp(wire+offsets[3],&expected_quant,sizeof expected_quant));
    assert(wire[offsets[3]]==0x6b && wire[offsets[3]+1]==0x05);
    assert(wire[offsets[3]+8]==15 && wire[offsets[3]+9]==15 && wire[offsets[3]+10]==15);
    for(unsigned i=0;i<452;i++)assert(!wire[offsets[4]+i]);
    for(unsigned off=0;off<128;off+=4) {
        u32 expected=off==4?0x03fc0000u:off==52?0x10180000u:0u;
        assert(wire32(wire+offsets[2]+off)==expected); // normal 16x16 intra; no IPCM/early shortcuts
    }
    /* Check the actual emitted addresses, not a duplicate of the production enum. */
    assert(addresses[0x71c/4]==VA_NVENC+0xb000);
    assert(addresses[0x720/4]>=addresses[0x71c/4]+cfg->pic_control.bitstream_buf_size);
    /* Linux explicitly zero-fills the logical history extent. Start each
     * scenario with dirty modeled VRAM, so this cannot pass merely because
     * static storage happened to start zero. Use the emitted binding. */
    assert(addresses[0x720/4]>=VA_NVENC);
    u64 history=addresses[0x720/4]-VA_NVENC;
    assert(history<=sizeof memory && cfg->pic_control.hist_buf_size<=sizeof memory-history);
    assert(history==0x4f000 && addresses[0x728/4]-VA_NVENC==history+0x18000);
    for(unsigned i=0;i<0x18000;i++)assert(memory[history+i]==0);
    assert(addresses[0x728/4]>=addresses[0x720/4]+cfg->pic_control.hist_buf_size);
    assert(addresses[0x72c/4]>=addresses[0x728/4]+0x8000);
    u64 input=addresses[0x734/4]-VA_NVENC_TILED,uv=addresses[0x740/4]-VA_NVENC_TILED;
    u64 output=addresses[0x730/4]-VA_NVENC_TILED,outuv=addresses[0x74c/4]-VA_NVENC_TILED;
    assert(input>=addresses[0x72c/4]-VA_NVENC+0x8000 && uv>=input+0x10000);
    assert(output>=uv+0x8000 && outuv>=output+0x10000 && outuv+0x8000<=sizeof memory);
    for(unsigned i=0;i<0x10000;i++)assert(memory[input+i]==0x80);
    for(unsigned i=0;i<0x8000;i++)assert(memory[uv+i]==0x80);
    if (scenario == 1) { ch->submit_failed = true; return false; }
    if (scenario == 2) return true; /* unchanged poisoned status */
    nvenc_pic_stat_s st = {0};
    st.error_status = 2; /* independent source-backed completed-state oracle */
    st.pic_type = 3; st.num_slices = 1;
    st.total_bit_count = 64; st.last_valid_byte_offset = 7;
    if (scenario == 3) st.total_bit_count = UINT32_MAX;
    if (scenario == 4) st.last_valid_byte_offset = UINT32_MAX;
    if (scenario == 5) st.total_bit_count = 0;
    if (scenario == 6) st.error_status = 1;
    if (scenario == 13) st.ucode_error_status = 1;
    if (scenario == 34) st.error_status = 0; /* no completion state */
    if (scenario == 35) st.error_status = 3; /* not the completed-picture state */
    if (scenario >= 36 && scenario <= 42) st.total_bit_count = 64 + scenario - 35;
    if (scenario == 43) st.last_valid_byte_offset = 4095; /* partial cursor cannot enlarge final output */
    if (scenario == 14) st.total_bit_count = cfg->pic_control.bitstream_buf_size*8+1;
    if (scenario == 15) st.last_valid_byte_offset = cfg->pic_control.bitstream_buf_size;
    if (scenario == 18) {st.total_bit_count=100000*8;st.last_valid_byte_offset=99999;}
    if (scenario == 19) {st.total_bit_count=cfg->pic_control.bitstream_buf_size*8;
                         st.last_valid_byte_offset=cfg->pic_control.bitstream_buf_size-1;}
    if (scenario == 20) {st.total_bit_count=4104*8;st.last_valid_byte_offset=4103;}
    if (scenario == 21) {st.total_bit_count=32*8;st.last_valid_byte_offset=31;}
    if (scenario == 24) {st.total_bit_count=20*8;st.last_valid_byte_offset=19;}
    if (scenario == 25) {st.total_bit_count=5*8;st.last_valid_byte_offset=4;}
    if (scenario == 26) {st.total_bit_count=7*8;st.last_valid_byte_offset=6;}
    if (scenario == 27) st.picture_index = 1;
    if (scenario == 28) st.pic_type = 0;
    if (scenario == 29) st.num_slices = 0;
    if (scenario == 30) st.num_slices = 2;
    if (scenario == 31) st.bitstream_start_pos = 4;
    memcpy(memory+0x3000, &st, sizeof st);
    static const u8 nal[] = {0,0,0,1,0x65,0x88,0x80,0};
    memcpy(memory+0xb000, nal, sizeof nal);
    if (scenario == 7) memory[0xb004] = 0x67; /* SPS only */
    if (scenario == 8) memory[0xb004] = 0xe5; /* forbidden bit */
    if (scenario == 16) { memory[0xb002] = 1; memory[0xb003] = 0x65; }
    if (scenario == 17) memory[0xb004] = 0x61; /* not the requested IDR */
    if (scenario == 20){
        /* Valid IDR beyond the former 2-KiB inspection limit, following SEI. */
        memory[0xb004]=0x06;memset(memory+0xb005,0x55,4096-5);
        memcpy(memory+0xc000,nal,sizeof nal);
    }
    if (scenario == 21) memcpy(memory+0xb010,nal,sizeof nal); /* second IDR rejected */
    if (scenario == 22) memory[0xb004]=0x05; /* IDR cannot be non-reference */
    if (scenario == 23) memory[0xb000]=0x99; /* garbage before prefix */
    if (scenario == 24){
        memcpy(memory+0xb008,nal,sizeof nal);memory[0xb004]=0x06;
        memory[0xb010]=0;memory[0xb011]=0;memory[0xb012]=1;memory[0xb013]=0x67; /* truncated following SPS */
    }
    return true;
}
'''
    preamble += function(SRC, "nvenc_transport_gate") + "\n"
    preamble += function(SRC, "nvenc_surface") + "\n"
    preamble += function(SRC, "nvenc_encode_frame_hw") + "\n"
    preamble += function(SRC, "nv_nvenc_selftest_hw")
    preamble += "\n" + function(SRC, "nv_nvenc_test_bitstream")
    preamble += r'''
int main(void) {
    for (scenario = 0; scenario <= 43; scenario++) {
        memset(ring, 0, sizeof ring); sem = 0;
        memset(memory,0x5a,sizeof memory);
        channels[0] = (nv_channel_t){.open=true, .sem=&sem, .pushbuf=(u8 *)ring};
        submits = reads = writes = output_read_bytes = 0;
        sequence=0;g_nvenc_transport_ready=false;
        g_nvenc_output.bytes=123;g_nvenc_output.slice_start=7;g_nvenc_output.slice_end=99;
        int rc = nv_nvenc_selftest_hw();
        bool pass=scenario == 0 || scenario == 4 || scenario == 15 || scenario == 16 || scenario == 18 || scenario == 19 || scenario == 20 || scenario == 26 || scenario == 43;
        assert((rc == 0) == pass);
        const u8 *stream=(const u8 *)1;u32 bytes=999,start=999,end=999;
        assert(nv_nvenc_test_bitstream(&stream,&bytes,&start,&end)==pass);
        if(pass){
            assert(stream==g_nvenc_output.data&&bytes&&end<=bytes&&start<end);
            assert(output_read_bytes==((bytes+3u)&~3u));
            nvenc_pic_stat_s completed;
            memcpy(&completed,memory+0x3000,sizeof completed);
            assert(bytes==completed.total_bit_count/8);
            if(scenario==4||scenario==15||scenario==43)assert(bytes==8&&output_read_bytes==8);
            assert(!memcmp(stream,memory+0xb000,bytes));
            assert(!memcmp(&g_nvenc_output_context,memory+0x1000,sizeof g_nvenc_output_context));
            if(scenario==20)assert(start==4096&&end==4104);
            if(scenario==19)assert(bytes==0x40000);
            if(scenario==26)assert(bytes==7&&output_read_bytes==8);
        }else assert(!stream&&!bytes&&!start&&!end);
        if (scenario == 1 || scenario == 9 || scenario == 10) assert(reads == 0);
        if (scenario == 2 || scenario == 3 || scenario == 5 || scenario == 6 || scenario == 13 || scenario == 14) assert(reads == 1);
        if (scenario >= 27 && scenario <= 31) assert(reads == 1 && output_read_bytes == 0);
        if (scenario >= 34 && scenario <= 42) assert(reads == 1 && output_read_bytes == 0);
        if (scenario == 1) {
            assert(channels[0].submit_failed);
            assert(nv_nvenc_selftest_hw() == 1 && submits == 2 && writes == 1);
        }
        if(scenario==32){
            assert(channels[0].submit_failed&&!g_nvenc_transport_ready);
            assert(nv_nvenc_selftest_hw()==1&&submits==1&&!writes&&!reads);
        }
        if(scenario==33)assert(submits==1&&writes==1&&!reads&&g_nvenc_transport_ready);
        if(pass){
            /* Once retired, readiness does not resubmit or touch picture state. */
            u32 old_at=channels[0].pb_at;
            assert(nvenc_transport_gate(&channels[0])&&submits==2&&channels[0].pb_at==old_at);
        }
    }
    for(unsigned failure=0;failure<2;failure++){
        g_nvenc_output.bytes=123;channels[0].open=failure!=0;channels[0].submit_failed=failure!=0;
        assert(nv_nvenc_selftest_hw()!=0&&!g_nvenc_output.bytes); // even early exits revoke stale output
    }
    puts("NVENC production submission: isolated transport release, phase-specific quarantine, IPCM capacity, emitted addresses/surfaces, launch-before-release, exact byte-aligned completed output independent of partial cursor, late IDR, malformed framing, completion state/identity and stale-result failure gates PASS (46 cases; modeled hardware)");
    return 0;
}
'''
    # GNU-layout COFF gives the firmware records their kernel/SysV bitfield ABI;
    # linking the C-only harness to the Windows CRT still lets it run locally.
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-nvenc-test-") as tmp:
        c, obj, exe = (Path(tmp) / n for n in ("test.c", "test.obj", "test.exe"))
        c.write_text(preamble)
        subprocess.run([clang, "--target=x86_64-w64-windows-gnu", "-mno-ms-bitfields",
                        "-ffreestanding", "-std=c11", "-O1", "-Wall", "-Wextra",
                        "-c", str(c), "-o", str(obj)], check=True)
        subprocess.run([clang, str(obj), "-o", str(exe)], check=True)
        if compile_only:
            print("COMPILE ONLY: NVENC production submission harness linked; assertions NOT executed")
        else:
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-only', action='store_true',
                        help='compile/link only; never execute modeled firmware or production functions')
    main(parser.parse_args().compile_only)
