#!/usr/bin/env python3
"""Execute the candidate's actual C control flow with mocked hardware edges.

Does NOT emulate a GPU or prove monitor lock/codec execution. Covers storage
boundaries, exact NVIDIA HOST packet encoding, quarantine on timeout, and
preparation/publication order. Generated translation units live in temp dirs.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CHAN = (ROOT / "kernel/nv_chan.c").read_text()
CLIENT = (ROOT / "kernel/nvkms_kapi_client.c").read_text()


def function(src, name):
    match = re.search(r"^(?:static )?(?:bool|NvBool|NV_STATUS|int|void|u32|u64|rect_t|long) " + name +
                      r"\([^;]*?\)\s*\{", src, re.M)
    assert match, name
    # Function closing braces in these sources are unindented.
    end = src.index("\n}", match.end()) + 2
    return src[match.start():end]


def macro(name):
    return re.search(r"^#define " + name + r"\s+[^\n]+", CHAN, re.M)[0]


def run_test(source, name, include_dirs=(), *, compile_only=False):
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-host-test-") as tmp:
        c = Path(tmp) / (name + ".c")
        exe = Path(tmp) / (name + ".exe")
        c.write_text(source)
        subprocess.run([clang, "-std=c11", "-O1", "-Wall", "-Wextra",
                        *[arg for path in include_dirs for arg in ('-I', str(path))],
                        str(c), "-o", str(exe)], check=True)
        if compile_only:
            print(f'COMPILED ONLY {name}: executable was not run')
        else:
            subprocess.run([str(exe)], check=True)


def test_ring():
    official = (ROOT / "out/nvidia-open-595.99.02/src/common/sdk/nvidia/inc/class/clc46f.h").read_text()
    assert re.search(r"NVC46F_SEM_ADDR_LO\s+\(0x0000005c\)", official)
    assert re.search(r"NVC46F_SEM_EXECUTE_RELEASE_WFI\s+20:20", official)
    assert re.search(r"NVC46F_SEM_EXECUTE_OPERATION_RELEASE\s+0x00000001", official)
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct {
    u32 pb_at, completion_seq, method_wraps;
    bool submit_failed;
    volatile u32 *sem;
    const char *name;
    volatile uint8_t *pushbuf;
} nv_channel_t;
#define kinfo(...) ((void)0)
#define kerr(...) ((void)0)
static unsigned submits;
static bool complete = true;
'''
    source += "\n".join(macro(n) for n in (
        "PB_METHOD_BEGIN", "PB_TRACKER_OFF", "HOST_SEM_ADDR_LO",
        "HOST_SEM_RELEASE_WFI", "VA_SEM"))
    source += "\n" + function(CHAN, "pb_method") + "\n" + function(CHAN, "pb_data")
    source += r'''
static u32 next_completion_signal(nv_channel_t *ch, u32 tag) {
    return tag | ++ch->completion_seq;
}
static bool submit_and_wait(nv_channel_t *ch, u32 off, u32 value) {
    submits++;
    assert(off == PB_TRACKER_OFF);
    assert(ch->pb_at == PB_TRACKER_OFF + 24);
    const volatile u32 *p = (const volatile u32 *)(ch->pushbuf + off);
    assert(p[0] == ((1u << 29) | (5u << 16) | (0x5cu >> 2)));
    assert(p[1] == (u32)VA_SEM && p[2] == (u32)(VA_SEM >> 32));
    assert(p[3] == value && p[4] == 0 && p[5] == 0x100001u);
    assert(ch->sem[0] == 0);
    ch->submit_failed = !complete;
    if (complete) ch->sem[0] = value;
    return complete;
}
'''
    source += function(CHAN, "pb_retire_and_wrap") + "\n" + function(CHAN, "pb_reserve")
    source += r'''
int main(void) {
    static uint8_t memory[0x80000];
    volatile u32 sem[4] = {0};
    nv_channel_t ch = {.pb_at=PB_METHOD_BEGIN, .sem=sem, .pushbuf=memory};
    memset(memory, 0xa5, sizeof memory);
    assert(pb_reserve(&ch, 96) && submits == 0);
    ch.pb_at = PB_TRACKER_OFF - 96;
    assert(pb_reserve(&ch, 96) && submits == 0); /* exact end is legal */
    assert(!pb_reserve(&ch, 0) && !pb_reserve(&ch, 3));
    assert(!pb_reserve(&ch, UINT32_MAX - 3));
    assert(!pb_reserve(&ch, PB_TRACKER_OFF - PB_METHOD_BEGIN + 4));
    assert(submits == 0);
    ch.pb_at = PB_TRACKER_OFF - 4;
    assert(pb_reserve(&ch, 96) && submits == 1);
    assert(ch.pb_at == PB_METHOD_BEGIN && ch.method_wraps == 1);
    for (unsigned i=0; i<PB_METHOD_BEGIN; i++) assert(memory[i] == 0xa5);
    for (unsigned i=0x70000; i<sizeof memory; i++) assert(memory[i] == 0xa5);
    for (unsigned i=0; i<100000; i++) {
        assert(pb_reserve(&ch, 96));
        ch.pb_at += 96;
    }
    assert(ch.method_wraps > 20);
    ch.pb_at = PB_TRACKER_OFF;
    complete = false;
    assert(!pb_reserve(&ch, 96) && ch.submit_failed);
    unsigned old = submits, wraps = ch.method_wraps;
    assert(!pb_reserve(&ch, 96));
    assert(!pb_retire_and_wrap(&ch));
    assert(submits == old && wraps == ch.method_wraps);
    ch.submit_failed = false;
    ch.pb_at = 0;
    assert(!pb_reserve(&ch, 96));
    ch.pb_at = PB_TRACKER_OFF + 4;
    assert(!pb_reserve(&ch, 96));
    puts("PASS: actual C method-ring boundaries, 100000 reservations, HOST packet, timeout quarantine");
}
'''
    run_test(source, "ring")


def test_scenes():
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef bool NvBool;
#define NV_FALSE false
#define NV_TRUE true
#define kinfo(...) ((void)0)
typedef struct {
    bool ran, post_codec, two_d_pass, three_d_pass;
    unsigned active_displays, active_heads_mask;
    const char *failed_at;
} nvkms_kapi_accel_result_t;
static struct {unsigned front, head;} active[4];
static unsigned active_count;
static void *test_device;
static NvU64 now, shown2, shown3;
static unsigned events, fail_at;
static bool prepared[2];
static NvU64 timer_now_us(void) {return now;}
static bool prepare_visible_accel(nvkms_kapi_accel_result_t *r, bool three, unsigned slot) {
    (void)r;
    assert(events == (three ? 2u : 0u));
    events++;
    for (unsigned i=0;i<active_count;i++) assert(active[i].front != slot);
    now += 120000;
    prepared[slot] = true;
    return events != fail_at;
}
static bool show_visible_accel(nvkms_kapi_accel_result_t *r, bool three, unsigned slot) {
    (void)r;
    assert(events == (three ? 4u : 1u));
    events++;
    assert(prepared[slot]);
    if (events == fail_at) return false;
    for (unsigned i=0;i<active_count;i++) active[i].front = slot;
    if (three) shown3 = now; else shown2 = now;
    now += 500000;
    return true;
}
static bool observe_visible_until(NvU64 deadline) {
    assert(events == 3u || events == 5u);
    events++;
    assert(now <= deadline);
    now = deadline;
    return events != fail_at;
}
'''
    source += function(CLIENT, "nvkms_kapi_run_visible_accel_test")
    source += r'''
int main(void) {
    for (unsigned n=1; n<=4; n++) {
        for (unsigned failure=0; failure<=6; failure++) {
            memset(active,0,sizeof active);
            for (unsigned i=0;i<n;i++) active[i].head=i;
            active_count=n; test_device=&active;
            now=0; events=0; fail_at=failure;
            memset(prepared,0,sizeof prepared);
            nvkms_kapi_accel_result_t result;
            bool ok=nvkms_kapi_run_visible_accel_test(&result, false);
            assert(ok == (failure == 0));
            if (ok) {
                assert(events == 6 && result.two_d_pass && result.three_d_pass);
                assert(shown3-shown2 == 5000000);
                assert(now-shown3 == 8000000);
                assert(result.active_heads_mask == (1u<<n)-1);
            } else assert(result.failed_at != NULL);
        }
    }
    test_device=NULL;
    nvkms_kapi_accel_result_t result;
    assert(!nvkms_kapi_run_visible_accel_test(&result, false));
    puts("PASS: actual C scene sequencing for 1-4 monitors; all six injected failures; 5s 2D/8s 3D");
}
'''
    run_test(source, "scenes")


def test_patterns():
    # Check the actual private pattern routine, including padded strides and
    # per-head dimensions. Upload is mocked; untouched padding catches overruns.
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef bool NvBool;
#define NV_FALSE false
#define NV_TRUE true
typedef struct {
    struct {struct {NvU32 hVisible,vVisible;} timings;} mode;
    NvU32 pitch, *pixels;
} display;
static display active[4];
static unsigned uploaded;
static bool upload_display(display *d, NvU32 slot) {
    assert(d == &active[uploaded++] && slot == 1);
    return true;
}
'''
    source += function(CLIENT, "draw_phase")
    source += r'''
int main(void) {
    static NvU32 pixels[4][80*20];
    const NvU32 widths[4]={3,17,33,65}, heights[4]={7,9,19,5};
    const NvU32 solids[4]={0xff0000,0xff00,0xff,0xffffff};
    const NvU32 v[8]={0xffffff,0xffff00,0xffff,0xff00,0xff00ff,0xff0000,0xff,0};
    const NvU32 h[8]={0,0xff,0xff0000,0xff00ff,0xff00,0xffff,0xffff00,0xffffff};
    const NvU32 m[6]={0xff3030,0x30ff30,0x3030ff,0xffff30,0x30ffff,0xff30ff};
    for (unsigned i=0;i<4;i++) {
        active[i].pixels=pixels[i]; active[i].pitch=80*4;
        active[i].mode.timings.hVisible=widths[i];
        active[i].mode.timings.vVisible=heights[i];
    }
    for (unsigned phase=0;phase<8;phase++) {
        for (unsigned i=0;i<4;i++) for (unsigned j=0;j<80*20;j++) pixels[i][j]=0xa5a5a5a5;
        uploaded=0;
        assert(draw_phase(phase,4,1) && uploaded==4);
        for (unsigned i=0;i<4;i++) for (unsigned y=0;y<20;y++) for (unsigned x=0;x<80;x++) {
            NvU32 expected=0xa5a5a5a5;
            if (x<widths[i] && y<heights[i]) {
                if (phase<4) expected=solids[phase];
                else if (phase==4) expected=v[x*8/widths[i]];
                else if (phase==5) expected=h[y*8/heights[i]];
                else if (phase==6) expected=m[(x/64+y/64+i)%6];
                else expected=0;
            }
            assert(pixels[i][y*80+x]==expected);
        }
    }
    puts("PASS: actual C RGBW/bars/mosaic/neutral pixels on four differing padded surfaces");
}
'''
    run_test(source, "patterns")
    boot = function(CLIENT, "nvkms_kapi_run_display_test")
    order = [boot.index(token) for token in (
        "present_phase(7, count)", "timer_mdelay(10000)",
        "present_phase(0, count)", "timer_mdelay(500)",
        "timer_mdelay(4500)", "for (NvU32 phase = 1; phase < 7; phase++)")]
    # The boot routine can contain earlier unrelated 500ms sleeps. Search the
    # CRC settling interval relative to the common red start.
    order[3] = boot.index("timer_mdelay(500)", order[2])
    assert order == sorted(order)
    print("PASS: neutral acquisition precedes the common timed red/RGBW start")


def test_observation():
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef bool NvBool;
#define NV_FALSE false
#define NV_TRUE true
#define kerr(...) ((void)0)
static NvU64 now, cost;
static bool healthy=true;
static unsigned pulses;
static NvU64 timer_now_us(void) {return now;}
static bool nv_chan_render_keepalive(void) {now+=cost; pulses++; return healthy;}
static void timer_mdelay(NvU32 ms) {assert(ms<=500); now+=(NvU64)ms*1000;}
static void timer_udelay(NvU32 us) {assert(us<1000); now+=us;}
'''
    source += function(CLIENT, "observe_visible_until")
    source += r'''
int main(void) {
    for (unsigned d=1;d<6000000;d+=7919) {
        now=0; cost=137; pulses=0;
        assert(observe_visible_until(d));
        assert(now>=d && now<=d+cost);
        assert(pulses && pulses<14);
    }
    now=0; healthy=false;
    assert(!observe_visible_until(5000000));
    puts("PASS: actual C observation deadline/keepalive, fractional milliseconds, failure handling");
}
'''
    run_test(source, "observation")


if __name__ == "__main__":
    test_ring()
    test_scenes()
    test_patterns()
    test_observation()
