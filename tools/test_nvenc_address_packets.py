#!/usr/bin/env python3
"""Check ONLY production NVENC packet construction in host memory, never a codec.

No device access, firmware model, encode/decode workload, or VM is involved.
Source authenticity is checked separately by test_nvenc_cfb7_abi.py.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_gpu_stable_candidate import function

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "kernel/nv_chan.c").read_text()
    encoder = function(source, "nvenc_encode_frame_hw")
    # Extract only enum constants and the straight-line packet construction.
    # Do not call the encoder, upload buffers, submit, or emulate completion.
    layout_start = encoder.index("enum {")
    layout_end = encoder.index("};", layout_start) + 2
    packet_start = encoder.index("if (!pb_reserve(ch, 256))")
    packet_end = encoder.index('\n    kinfo("nv-chan", "NVENC: encoding', packet_start)
    packet = encoder[packet_start:packet_end]
    assert "submit_and_wait" not in packet and "nv_vram_object" not in packet
    code = r'''
#include <stdint.h>
#include <stddef.h>
extern int puts(const char *);
extern void abort(void);
void __main(void) {}
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { volatile unsigned char *pushbuf; u32 pb_at; } nv_channel_t;
#define check(x) do { if (!(x)) abort(); } while (0)
static u32 reserved_end, interrupt_enabled, reserve_allowed;
static int pb_reserve(nv_channel_t *ch, u32 bytes) {
    check(bytes==256 && ch->pb_at+bytes<=1024);
    reserved_end=ch->pb_at+bytes;
    return reserve_allowed;
}
static u32 next_completion_signal(nv_channel_t *ch,u32 tag) {
    (void)ch; check(tag==0x454e0000); return tag|2;
}
static u32 nv_completion_awaken(nv_channel_t *ch,u32 flag) {
    (void)ch; return interrupt_enabled ? flag : 0;
}
'''
    names = ["SUBCH_NVENC", "NV906F_SET_OBJECT_ENGINE_SW", "VA_NVENC",
             "VA_NVENC_TILED", "VA_SEM", "VIDEO_COMPLETION_AWAKEN"]
    names += re.findall(r"^#define (NVCFB7_\w+)\s", source, re.M)
    for name in dict.fromkeys(names):
        code += re.search(r"^#define\s+"+name+r"\s+[^\n]*", source, re.M)[0]+"\n"
    code += function(source, "pb_data") + "\n"
    code += function(source, "pb_method") + "\n"
    code += function(source, "nvenc_address") + "\n"
    code += '#include "'+(ROOT / 'kernel/nvenc_drv_h264.h').as_posix()+'"\n'
    code += "static int build_commands(nv_channel_t *ch) {\n"
    code += encoder[layout_start:layout_end]+"\n"+packet
    code += "\ncheck(ch->pb_at-start==220 && ch->pb_at<=reserved_end); return 0;\n}\n"
    code += r'''
int main(void) {
    const u32 methods[] = {0x710,0x718,0x71c,0x720,0x728,0x72c,
                          0x730,0x74c,0x734,0x740,0x744,0x70c,0x724};
    const u64 addresses[] = {0,0x800000000ull,0x800001000ull,
        0x880077000ull,0xffffffff00ull,0x10000000000ull,
        0x123456789ab00ull,0x1ffffffffffff00ull};
    for (unsigned m=0;m<sizeof methods/sizeof methods[0];m++) {
        for (unsigned a=0;a<sizeof addresses/sizeof addresses[0];a++) {
            u32 words[5] = {0xdeadbeef,0,0,0,0xcafebabe};
            nv_channel_t ch = {(volatile unsigned char *)words,4};
            nvenc_address(&ch,methods[m],addresses[a]);
            check(ch.pb_at==16 && words[0]==0xdeadbeef && words[4]==0xcafebabe);
            check(words[1]==(0x60028000u|methods[m]/4));
            check(words[2]==(u32)((addresses[a]/256)&0xffffffffull));
            check(words[3]==addresses[a]/0x10000000000ull);
            check((((u64)words[3]<<40)|((u64)words[2]<<8))==addresses[a]);
        }
    }
    /* Independent raw command oracle: class/control/fence fields plus the
     * thirteen actual native allocation addresses. Do not generate this with
     * the production constants or helper we are checking. */
    u32 expected[] = {
        0x20018000,0x001fcfb7,0x20018080,1,
        0x200181c0,0x1303,0x200181c1,0,
        0x600281c4,0x08000010,0,0x600281c6,0x08000030,0,
        0x600281c7,0x080000b0,0,0x600281c8,0x080004f0,0,
        0x600281ca,0x08000670,0,0x600281cb,0x080006f0,0,
        0x600281c9,0x08000b90,0,
        0x600281cc,0x088009f0,0,0x600281d3,0x08800af0,0,
        0x600281cd,0x088007f0,0,0x600281d0,0x088008f0,0,
        0x600281d1,0x088008f0,0,
        0x600281c3,0x08000b70,0,
        0x200180c0,0,0x20038090,2,0x00200000,0x454e0002,
        0x200180c1,0
    };
    _Static_assert(sizeof expected==220,"full packet oracle size");
    const u32 starts[]={0,4,128,768};
    for(reserve_allowed=0;reserve_allowed<2;reserve_allowed++) {
        for(interrupt_enabled=0;interrupt_enabled<2;interrupt_enabled++) {
            expected[54]=interrupt_enabled?0x100:0;
            for(unsigned s=0;s<4;s++) {
                u32 guarded[258];
                for(unsigned i=0;i<258;i++)guarded[i]=0xdefaced;
                nv_channel_t ch={(volatile unsigned char *)(guarded+1),starts[s]};
                check(build_commands(&ch)==(reserve_allowed?0:1));
                check(ch.pb_at==starts[s]+(reserve_allowed?220:0));
                for(unsigned i=0;i<258;i++) {
                    unsigned first=1+starts[s]/4;
                    if(reserve_allowed && i>=first && i<first+55)
                        check(guarded[i]==expected[i-first]);
                    else check(guarded[i]==0xdefaced);
                }
            }
        }
    }
    puts("PASS: 104 address packets + 16 complete command-construction cases (exact 220-byte stream, reservation failure, IRQ on/off, cursor and guards; host memory only, no codec executed)");
    return 0;
}
'''
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-nvenc-address-") as tmp:
        c, obj, exe = (Path(tmp) / name for name in ("packet.c", "packet.obj", "packet.exe"))
        c.write_text(code)
        subprocess.run([clang, "--target=x86_64-w64-windows-gnu", "-mno-ms-bitfields", "-ffreestanding",
                        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-c", str(c), "-o", str(obj)], check=True)
        subprocess.run([clang, str(obj), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
