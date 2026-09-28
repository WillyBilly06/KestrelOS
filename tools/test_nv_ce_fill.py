#!/usr/bin/env python3
"""Execute production CE fill/copy encoders against a command-decoding model.

The model checks byte footprints and command state, not real Blackwell execution.
"""
from pathlib import Path
import re
from test_gpu_stable_candidate import function, run_test

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "kernel/nv_chan.c").read_text()
CLIENT = (ROOT / "kernel/nvkms_kapi_client.c").read_text()


def macro(name):
    return re.search(r"^#define\s+" + name + r"\s+(?:[^\n]*\\\n)*[^\n]*", SRC, re.M)[0]


def test_readback_gate():
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
typedef uint32_t NvU32; typedef uint64_t NvU64; typedef bool NvBool;
#define NV_TRUE true
#define NV_FALSE false
#define kinfo(...) ((void)0)
#define kerr(...) ((void)0)
typedef struct { NvU32 hRmClient; } kapi_device_rm_prefix_t;
typedef struct { NvU32 hRmHandle; } kapi_memory_rm_prefix_t;
typedef struct { struct {struct {NvU32 hVisible, vVisible;} timings;} mode;
                 NvU32 head, pitch; void *memory[2]; } test_display_t;
static kapi_device_rm_prefix_t device = {123};
static void *test_device = &device;
static u32 g_runtime_scanout_width, g_runtime_scanout_height, pitch;
static u32 *pixels;
static int reads, bad_pixel = -1, fail_transfer = -1;
static bool nv_chan_display_ready(void) { return true; }
static bool nv_chan_fill_scanout(s32 x, s32 y, s32 w, s32 h, u32 colour) {
    assert(x >= 0 && y >= 0 && w > 0 && h > 0);
    assert((u32)(x+w) <= g_runtime_scanout_width && (u32)(y+h) <= g_runtime_scanout_height);
    for (int row = y; row < y+h; row++) for (int col = x; col < x+w; col++)
        pixels[row*pitch+col] = colour;
    return true;
}
static bool nv_chan_copy_scanout(s32 sx, s32 sy, s32 dx, s32 dy, s32 w, s32 h) {
    assert(sx >= 0 && sy >= 0 && dx >= 0 && dy >= sy+h);
    assert((u32)(dx+w) <= g_runtime_scanout_width && (u32)(dy+h) <= g_runtime_scanout_height);
    for (int row = 0; row < h; row++)
        memcpy(pixels+(dy+row)*pitch+dx, pixels+(sy+row)*pitch+sx, w*4);
    return true;
}
static bool nv_chan_present_image(const u32 *p, u32 w, u32 h, u32 stride, s32 x, s32 y) {
    assert(x >= 0 && y >= 0 && (u32)x+w <= g_runtime_scanout_width && (u32)y+h <= g_runtime_scanout_height);
    for (u32 row = 0; row < h; row++) memcpy(pixels+(y+row)*pitch+x, p+row*stride, w*4);
    return true;
}
static NvU32 nvrm_transfer_rm_memory(NvU32 client, NvU32 mem, NvU64 offset,
                                    void *out, NvU64 bytes, NvBool read) {
    assert(client == 123 && mem == 456 && bytes == 4 && read);
    assert(offset % 4 == 0 && offset/4 < (u64)pitch*g_runtime_scanout_height);
    int index = reads++;
    if (index == fail_transfer) return 1;
    *(u32 *)out = pixels[offset/4] ^ (index == bad_pixel ? 1u : 0u);
    return 0;
}
'''
    source += function(SRC, "nv_chan_visible_2d_hw") + "\n"
    source += function(CLIENT, "verify_visible_2d_fill")
    source += r'''
int main(void) {
    const u32 dims[4][2] = {{320,240},{853,479},{2560,1440},{3840,2160}};
    kapi_memory_rm_prefix_t mem = {456};
    for (unsigned n = 0; n < 4; n++) {
        g_runtime_scanout_width = dims[n][0]; g_runtime_scanout_height = dims[n][1];
        pitch = g_runtime_scanout_width + 13;
        pixels = calloc((size_t)pitch*g_runtime_scanout_height, 4); assert(pixels);
        test_display_t d = {.mode = {.timings = {dims[n][0],dims[n][1]}},
                             .head = n, .pitch = pitch*4, .memory = {&mem,&mem}};
        assert(nv_chan_visible_2d_hw() == 0);
        for (unsigned slot = 0; slot < 2; slot++) {
            reads = 0; bad_pixel = fail_transfer = -1;
            assert(verify_visible_2d_fill(&d, slot) && reads == 12);
            for (int i = 0; i < 12; i++) {
                reads = 0; bad_pixel = i;
                assert(!verify_visible_2d_fill(&d, slot) && reads == i+1);
                reads = 0; bad_pixel = -1; fail_transfer = i;
                assert(!verify_visible_2d_fill(&d, slot) && reads == i+1);
                fail_transfer = -1;
            }
        }
        free(pixels);
    }
    puts("PASS: actual 2D scene/readback gate, four resolutions, both slots, every pixel/transfer failure");
    return 0;
}
'''
    run_test(source, "ce_fill_readback")


def main():
    ref = (ROOT / "out/nvidia-open-595.99.02/src/common/sdk/nvidia/inc/class/clc8b5.h").read_text()
    expected = {"SET_REMAP_CONST_B": 0x704, "SET_REMAP_COMPONENTS": 0x708,
                "SET_REMAP_COMPONENTS_DST_X_CONST_B": 5,
                "SET_REMAP_COMPONENTS_COMPONENT_SIZE_FOUR": 3,
                "SET_REMAP_COMPONENTS_NUM_DST_COMPONENTS_ONE": 0}
    for name, value in expected.items():
        got = re.search(r"#define NVC8B5_" + name + r"\s+\(0x([0-9A-Fa-f]+)\)", ref)
        assert got and int(got[1], 16) == value, name
    assert re.search(r"NVC8B5_LAUNCH_DMA_REMAP_ENABLE\s+10:10", ref)
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
typedef struct { bool open, submit_failed; u32 pb_at, seq; volatile u32 *sem;
                 volatile uint8_t *pushbuf; } nv_channel_t;
#define CH_COPY 0
static nv_channel_t channels[1];
static bool g_scanout_bound = true;
static u64 g_runtime_scanout_va = 0x100000000ull;
static u32 g_runtime_scanout_width = 53, g_runtime_scanout_height = 37;
static u32 g_runtime_scanout_pitch = 256;
static uint8_t memory[256*40], expected_memory[256*40];
static uint8_t *active_memory=memory;
static size_t active_bytes=sizeof memory;
static u32 pb[64], sem[1], registers[0x800/4];
static unsigned submits, reserves;
static bool retire = true, reserve_ok = true, execute_pixels = true;
static u32 budget;
static bool pb_reserve(nv_channel_t *ch, u32 bytes) {
    reserves++; assert(ch->pb_at == 0); budget = bytes;
    return reserve_ok && !ch->submit_failed;
}
static u32 next_completion_signal(nv_channel_t *ch, u32 tag) { return tag | ++ch->seq; }
/* This command model has no scheduler/IRQ route; keep boot-style polling. */
static u32 nv_completion_awaken(nv_channel_t *ch, u32 flag) { (void)ch; (void)flag; return 0; }
'''
    for name in ("BLACKWELL_DMA_COPY_B", "SUBCH_COPY", "VA_SEM", "CE_OFFSET_IN_UPPER",
                 "CE_OFFSET_OUT_UPPER", "CE_PITCH_IN", "CE_LINE_LENGTH_IN",
                 "CE_SET_SEMAPHORE_A", "CE_LAUNCH_DMA", "CE_SET_REMAP_CONST_B",
                 "CE_SET_REMAP_COMPONENTS", "CE_REMAP_FILL32", "CE_REMAP_ENABLE", "CE_LAUNCH",
                 "CE_COMPLETION_AWAKEN"):
        source += macro(name) + "\n"
    source += function(SRC, "pb_method") + "\n" + function(SRC, "pb_data")
    source += r'''
static bool submit_and_wait(nv_channel_t *ch, u32 start, u32 signal) {
    assert(start == 0 && ch->pb_at <= budget && sem[0] == 0);
    submits++;
    for (u32 i = 0; i < ch->pb_at / 4;) {
        u32 head = pb[i++], count = (head >> 16) & 0x1fffu, method = (head & 0x1fffu) * 4;
        assert(head >> 29 == 1 && ((head >> 13) & 7) == SUBCH_COPY);
        assert(i + count <= ch->pb_at / 4);
        for (u32 n = 0; n < count; n++, method += 4) {
            assert(method < sizeof registers); registers[method / 4] = pb[i++];
        }
    }
    assert(registers[0] == BLACKWELL_DMA_COPY_B);
    assert(registers[0x240/4] == VA_SEM >> 32 && registers[0x244/4] == (u32)VA_SEM);
    assert(registers[0x248/4] == signal);
    u32 launch = registers[0x300/4];
    assert((launch & ~CE_REMAP_ENABLE) == CE_LAUNCH);
    u64 dst = ((u64)registers[0x408/4] << 32) | registers[0x40c/4];
    u64 src = ((u64)registers[0x400/4] << 32) | registers[0x404/4];
    u32 width = registers[0x418/4], height = registers[0x41c/4];
    bool fill = !!(launch & CE_REMAP_ENABLE);
    if (fill) {
        assert(ch->pb_at == 80 && budget == 80);
        assert(registers[0x708/4] == (5u | (3u << 16)));
        assert(registers[0x410/4] == width * 4);
    }
    if (retire && execute_pixels) {
        for (u32 y = 0; y < height; y++) {
            u64 off = dst - g_runtime_scanout_va + (u64)y * registers[0x414/4];
            u32 bytes = fill ? width * 4 : width;
            assert(off + bytes <= active_bytes);
            if (fill) {
                for (u32 x = 0; x < width; x++)
                    memcpy(active_memory + off + x * 4, &registers[0x704/4], 4);
            } else {
                u64 in = src - g_runtime_scanout_va + (u64)y * registers[0x410/4];
                assert(in + bytes <= active_bytes);
                memcpy(active_memory + off, active_memory + in, bytes);
            }
        }
    }
    ch->submit_failed = !retire;
    if (retire) sem[0] = signal;
    ch->pb_at = 0;
    return retire;
}
'''
    for name in ("ce_copy", "ce_fill32", "nv_chan_display_ready", "nv_chan_fill_scanout"):
        source += function(SRC, name) + "\n"
    source += r'''
int main(void) {
    nv_channel_t *ch = &channels[0]; ch->open = true; ch->pushbuf = (void *)pb; ch->sem = sem;
    u32 rng = 12345;
    /* Exhaustive 1-pixel destinations, then varied padded rectangles/colours. */
    for (int n = 0; n < 3000; n++) {
        rng = rng * 1664525u + 1013904223u;
        u32 x = n < 1961 ? n % 53 : rng % 53;
        u32 y = n < 1961 ? n / 53 : (rng >> 16) % 37;
        u32 w = n < 1961 ? 1 : 1 + rng % (53 - x);
        u32 h = n < 1961 ? 1 : 1 + (rng >> 8) % (37 - y);
        memset(memory, 0xa5, sizeof memory); memcpy(expected_memory, memory, sizeof memory);
        for (u32 row = y; row < y + h; row++)
            for (u32 col = x; col < x + w; col++)
                memcpy(expected_memory + row * 256 + col * 4, &rng, 4);
        assert(nv_chan_fill_scanout(x, y, w, h, rng));
        assert(memcmp(memory, expected_memory, sizeof memory) == 0);
    }
    /* Persistent remap state MUST NOT turn the next byte copy into a fill. */
    for (int row = 0; row < 3; row++)
        memcpy(expected_memory + (row + 5) * 256 + 80, memory + row * 256, 16);
    assert(ce_copy(ch, g_runtime_scanout_va, g_runtime_scanout_va + 5*256 + 80,
                   256, 256, 16, 3, 0x434f0000));
    assert(!(registers[0x300/4] & CE_REMAP_ENABLE));
    assert(memcmp(memory, expected_memory, sizeof memory) == 0);
    /* New surface initialization: real CAB5 packets at high surface VAs,
     * through the full 256-MiB supported allocation limit, with guard bytes. */
    const size_t allocations[]={65536,4194304,44236800,0x10000000};
    for(u32 i=0;i<4;i++) {
        size_t bytes=allocations[i];active_bytes=bytes+128;
        active_memory=malloc(active_bytes);assert(active_memory);
        memset(active_memory,0xa5,active_bytes);
        g_runtime_scanout_va=0x30000000000ull+(u64)i*0x10000000ull-64;
        assert(ce_fill32(ch,g_runtime_scanout_va+64,16384,4096,(u32)(bytes/16384),0,0x53430000));
        for(size_t at=0;at<active_bytes;at++)
            assert(active_memory[at]==(at<64 || at>=bytes+64 ? 0xa5 : 0));
        free(active_memory);
    }
    active_memory=memory;active_bytes=sizeof memory;g_runtime_scanout_va=0x100000000ull;
    unsigned before = submits;
    assert(!nv_chan_fill_scanout(-1, 0, 1, 1, 0));
    assert(!nv_chan_fill_scanout(0, 0, 54, 1, 0));
    assert(!nv_chan_fill_scanout(0, 37, 1, 1, 0));
    assert(!nv_chan_fill_scanout(52, 36, 2, 2, 0));
    assert(!nv_chan_fill_scanout(0, 0, 0x7fffffff, 1, 0));
    assert(!ce_fill32(ch, 3, 256, 1, 1, 0, 1));
    assert(!ce_fill32(ch, 4, 255, 1, 1, 0, 1));
    assert(!ce_fill32(ch, 4, 4, 2, 1, 0, 1));
    assert(!ce_fill32(ch, UINT64_MAX - 3, 256, 1, 2, 0, 1));
    assert(!ce_fill32(ch, (1ull << 49) - 4, 256, 2, 1, 0, 1));
    assert(submits == before);
    execute_pixels = false;
    assert(ce_fill32(ch, 0x180000000ull, 4096*4, 3840, 2160, 0xff123456, 0x46490000));
    assert(registers[0x418/4] == 3840 && registers[0x41c/4] == 2160);
    reserve_ok = false; before = submits;
    assert(!nv_chan_fill_scanout(0, 0, 1, 1, 0) && submits == before);
    reserve_ok = true; retire = false;
    assert(!nv_chan_fill_scanout(0, 0, 1, 1, 0) && ch->submit_failed);
    before = submits; unsigned old_reserves = reserves;
    assert(!nv_chan_fill_scanout(0, 0, 1, 1, 0));
    assert(submits == before && reserves == old_reserves);
    puts("PASS: actual CAB5 fill encoder, 3000 guarded rectangles, high-VA allocation clears 64KiB through 256MiB, copy-state reset, timeout quarantine");
    return 0;
}
'''
    run_test(source, "ce_fill")
    test_readback_gate()


if __name__ == "__main__":
    main()
