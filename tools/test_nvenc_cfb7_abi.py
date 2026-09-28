#!/usr/bin/env python3
"""Check CFB7 record layout against the supplied 595.99.02 ELF instructions.

Reads but never executes the driver. Runs a host harness for Kestrel's structure
and timer helper. Neither this check nor the submission model proves NVENC works
on the physical card; optional extension-field semantics remain incomplete.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "Linux NVIDIA Driver/libnvcuvid.so.595.99.02"
SHA256 = "9a236561b562325e6ec4ca4430754778d4c59ac72147dfab488550bf00378233"


def reference_check():
    captured = json.loads((ROOT / 'tools/fixtures/nvenc_cfb7_windows_256.json').read_text())
    picture = bytes.fromhex(captured['picture_hex'])
    assert len(picture) == 768
    assert hashlib.sha256(picture).hexdigest() == captured['picture_sha256']
    assert struct.unpack_from('<I', picture)[0] == 0xcfb70006
    assert picture[0x1ae] & 7 == 0, 'successful H.264 firmware selector must not become outer command codec 3'
    assert picture[0x1c2:0x1c8] == bytes(6), 'full-frame capture must not request strip processing'
    assert picture[0xba] == 1, 'SDK single-pass CQP uses firmware FIRST_PASS=1'
    assert struct.unpack_from('<I', picture, 0x19c)[0] == 6144, 'CFB7 width-derived history extent'
    for offset in (4, 36, 68):
        assert struct.unpack_from('<HH', picture, offset) == (255, 255)
    blob = LIB.read_bytes()  # Missing reference is a failure, not a verification pass.
    assert hashlib.sha256(blob).hexdigest() == SHA256, "new binary requires a fresh ABI audit"
    assert blob[:6] == b"\x7fELF\x02\x01"
    phoff = struct.unpack_from("<Q", blob, 32)[0]
    entsize, count = struct.unpack_from("<HH", blob, 54)
    loads = []
    for i in range(count):
        kind, flags, off, va, _, filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", blob, phoff + i * entsize)
        if kind == 1:
            loads.append((va, off, filesz, flags))

    def code(va, n, executable=True):
        for base, off, size, flags in loads:
            if (not executable or flags & 1) and base <= va and va + n <= base + size:
                return blob[off + va - base:off + va - base + n]
        raise AssertionError(f"unmapped executable VMA {va:#x}")

    # ELF VMAs, not Ghidra's addresses (which are rebased by +0x100000).
    # Keep instruction checks at the actual audited producer/converter/upload,
    # not an anywhere-in-binary magic-number search.
    anchors = {
        # Constructor pass selector, then its byte copy into the picture.
        # SDK CQP/single-pass still uses firmware FIRST_PASS=1, not zero.
        0xccd2f: "48 c7 83 78 0a 00 00 01 00 00 00",
        0xd12fa: "41 8b 87 78 0a 00 00",
        0xd130f: "41 88 87 b6 67 00 00",
        # RC input allocation: padded row stats + 256-byte header + QP rows.
        0xcc2d5: "8d 58 07 c1 e0 08 83 e3 f8",
        0xcc2e2: "c1 e3 04 81 c3 ff 00 00 00 30 db",
        0xcc2f8: "8d b4 03 00 01 00 00",
        # CQP explicitly preserves the 1.0 fixed-point ratio for P/B/I.
        0xdc89e: "41 89 fb 41 83 e3 0f",
        0xdc8cf: "b8 00 01 00 00 45 85 d2 74 22 45 85 db 74 1d",
        0xdc8fb: "89 84 93 08 6c 00 00 48 83 c2 01 48 83 fa 03 75 99",
        # H.264 output reader: raw mapped status +4 bits[1:0] must be 2
        # before total_bit_count at +8 is consumed as complete output bytes.
        0xbc314: "45 0f b6 6c 24 04 41 83 e5 03",
        0xbbf67: "41 83 fd 02 0f 84 bf 05 00 00",
        0xbc530: "41 8b 7c 24 08 c1 ef 03 03 7d 24 89 7d 24",
        # Reject non-byte-aligned counts before either output-length path.
        0xbc19a: "41 8b 44 24 08 75 08 85 c0 0f 84 ef 06 00 00",
        0xbc1a9: "83 e0 07 89 44 24 40 0f 85 e2 06 00 00",
        # Extended address binding, not merely the picture producer. Newer
        # channel classes take the two-word path even for a zero high word.
        # This was independently cross-checked against Windows 610.62.
        0x48f20: "49 c1 ee 08 48 c1 eb 28",
        0x48f28: "81 bd 90 00 00 00 6f c8 00 00 77 09 48 85 db 0f 84 e3 00 00 00",
        0x48f63: "41 b8 02 00 00 00 44 89 f9 44 89 e6 8b 95 ac 00 00 00 48 8b 07 ff 50 50",
        0x48f82: "44 89 f2 44 89 e6 48 8b 07 ff 50 48",
        0x48f95: "89 da 44 89 e3 44 89 e6 48 8b 07 ff 50 48",
        # The concrete Linux writer emits NON_INCREMENTING (opcode 3), not
        # an increment to the adjacent firmware method after the low word.
        0x47aa3: "81 ca 00 00 00 60 89 10",
        # History initialization: session passes offset=0 and logical size;
        # encoder forwards fill value=0 through backend vtable +0x1a0.
        0xc7a80: "48 8b 7b 38 8b 8b ec 02 00 00 45 31 c0 31 d2 48 8b 07 ff 50 68",
        0x1275b4: "48 8b 7f 28 4d 89 c1 48 8b 07 48 8b 80 a0 01 00 00",
        0x1275e8: "6a 00 4c 8b 8b 08 03 00 00 45 31 c0 6a 00 ff d0",
        0x2da41: "e9 5a fc ff ff",  # bounds-checked dispatch to fill builder
        # CE SET_REMAP_CONST_A receives the fill value; 0x6664 selects one
        # one-byte CONST_A output component. No hidden nonzero history seed.
        0x2d72d: "b8 00 07 00 00",
        0x2d7a0: "44 89 84 24 a4 00 00 00 b8 90 00 00 00 b9 64 66 00 00",
        0x2d7ba: "ba 08 07 00 00",
        # Newer-video picture producer: width in MBs *3*128, round to256,
        # write record+0x19c. It does not read the backing allocation capacity.
        0xd1470: "e8 8b a7 ff ff 84 c0 0f 84 fb 06 00 00",
        0xd147d: "41 8b 87 54 04 00 00 8d 04 40 c1 e0 07 05 ff 00 00 00 30 c0",
        0xd1499: "41 89 87 98 68 00 00",
        # Normal full-frame backing allocation is a 16-slot pool, distinct
        # from the above per-picture size. Explicit strip mode can override
        # the capacity later; it is not enabled by the native full-frame path.
        0xd69df: "c6 85 58 13 00 00 01 c7 85 d8 a7 00 00 10 00 00 00",
        0xd6a3c: "80 bd 38 06 00 00 00 0f 85 87 0c 00 00",
        0xe2408: "44 8b bd d8 a7 00 00",
        0xe2442: "b9 02 00 00 00 84 c0 0f 84 21 05 00 00",
        0xe2898: "0f af ca 8d 04 49 c1 e0 06 05 ff 00 00 00 30 c0 41 0f af c7 41 89 c4",
        0xe2481: "44 89 64 24 24",
        0xca2aa: "f3 0f 6f 53 10 8b 85 d8 02 00 00 0f 11 95 e8 02 00 00",
        0xca544: "8b b5 ec 02 00 00 85 f6 0f 85 5a 01 00 00",
        0xca6f2: "e8 d9 64 09 00 48 89 45 60",
        # RC work state: allocation extent and both alternating resources;
        # first-picture CPU initialization produces records at 0 and 0x100.
        0xe248f: "c1 e0 08 8d 84 00 00 0c 00 00 89 44 24 3c",
        0xca55c: "89 b5 c8 01 00 00 48 8b 7d 08",
        0xca573: "e8 58 66 09 00 48 89 85 b8 01 00 00",
        0xca59f: "e8 2c 66 09 00 48 89 85 c0 01 00 00",
        0xde732: "44 8b 9d 20 04 00 00 45 85 db 74 10",
        0xde79a: "e8 41 a6 fe ff",
        0xde7b5: "49 8d b4 24 00 01 00 00 e8 1e a6 fe ff",
        0xc8e56: "c7 44 24 d8 00 01 00 00",
        0xc8e80: "48 b8 18 00 00 00 0c 00 00 00",
        0xc8e95: "48 b8 30 00 00 00 18 00 00 00",
        0xc8ea4: "48 83 e8 18 48 89 44 24 a0",
        0xc8eb3: "48 b8 0c 00 00 00 0c 00 00 00",
        0xc8ec2: "48 b8 0c 00 00 00 30 00 00 00",
        0xc8ed1: "48 83 c0 24",
        0xc8efa: "b8 1a 00 00 00",
        # Ordinary emitter only tests resource presence; no CQP bypass here.
        0x120369: "48 8b 55 78 48 85 d2 74 12 45 31 c0 31 c9 be 24 07 00 00 4c 89 e7 e8 ec b6 00 00",
        # Full-frame and strip paths are distinct. Session byte +0x1359 is
        # set when strip count >1, and gates all strip ID/start/count stores.
        0xd6a49: "0f b6 85 58 13 00 00 3c 01",
        0xd6a58: "0f 97 85 59 13 00 00",
        0xde076: "0f b6 bb 59 13 00 00",
        0xde136: "40 84 ff 74 48",
        0xde145: "88 83 be 68 00 00",
        0xde153: "66 44 89 83 c0 68 00 00",
        0xde17c: "66 89 83 c2 68 00 00",
        # Video fence helper: address/payload then a standalone flushing
        # SEMAPHORE_D=0 release on subchannel 4 (not EXECUTE configuration).
        0x22fe3: "be 04 00 00 00 ba 40 02 00 00 e8 ee 89 ff ff",
        0x2302d: "41 83 fe 04 74 06 41 83 fe 1a 75 57",
        0x23090: "48 89 ef be 04 00 00 00 b9 01 00 00 00 ba 04 03 00 00 e8 39 89 ff ff 5b 48 89 ef 31 f6 5d 41 5c 41 5d 41 5e e9 a7 89 ff ff",
        # ME base initialization deliberately clears the 9-bit MBC-size field.
        # Do not replace zero with an invented nonzero "fix" for the timeout.
        0xcda58: "66 81 a7 44 07 00 00 00 fe",
        # The newer ME producer preserves bit 15, writes the five 3-bit hint
        # types 0,1,2,3,4 into bits 0..14, then stores at ME+0x9a.
        0xcfb97: "0f b7 83 9a 00 00 00 66 25 00 80 66 0d 88 46 66 89 83 9a 00 00 00",
        # Sequence frame-rate conversion: session fps -> multiply by 256.0
        # (RIP-relative double at 0x100a490) -> truncate -> sequence 0x6c14.
        # The picture producer copies it to record 0x88, not a 16.16 field.
        0xdc854: "f2 0f 10 15 34 dc f2 00",
        0xdc933: "f2 0f 10 a3 08 0b 00 00",
        0xdc955: "66 0f 28 c4",
        0xdc965: "f2 0f 59 c2 88 83 07 6c 00 00 f2 0f 2c c0 89 83 14 6c 00 00",
        0xd1222: "41 8b 87 14 6c 00 00 41 89 87 84 67 00 00",
        # CFB7 auxiliary upload: slice count times 128, then ME's last 16
        # bytes at +0xb0 and MD's last 16 bytes at +0x70.
        0xde3fc: "8b bb b8 03 00 00 48 8d b3 38 45 00 00 48 63 93 04 7b 00 00 4c 01 f7 48 c1 e2 07 e8 54 8f f3 ff",
        0xde4b8: "f3 0f 6f bb e8 43 00 00 0f 11 b8 b0 00 00 00",
        0xde4c7: "8b 83 b0 03 00 00 f3 0f 6f a3 f8 43 00 00 4c 01 f0 0f 11 20",
        0xde523: "f3 0f 6f bb 68 44 00 00 0f 11 78 70",
        # Ordinary encoder emitter: object +0x38 controls FORCE_OUT_COL and
        # is the very same object bound by method 0x72c, not another codec's
        # similarly named flags. ELF addresses are Ghidra addresses -0x100000.
        0x120014: "48 8b 45 38",
        0x120035: "31 db 48 85 c0 0f 95 c3 c1 e3 09",
        0x12007c: "f7 d3 81 e3 00 02 00 00",
        0x1201d0: "09 da 44 09 fa 41 c1 e5 14 09 ca 09 c2 0b 54 24 40 44 09 ea",
        0x1201eb: "be 00 07 00 00 4c 89 e7 e8 e8 b7 00 00",
        0x12030c: "48 8b 55 38 48 85 d2 74 13 8b 4d 44 45 31 c0 be 2c 07 00 00 4c 89 e7 e8 48 b7 00 00",
        # Missing DPB references become -2, then the picture producer copies
        # their low bytes to L0/L1. IDR (pic_type 3) skips temporal-distance work.
        0xec770: "c7 04 83 fe ff ff ff 48 83 c0 01 83 f8 1f 7e f0",
        0xd13a0: "41 8b 94 87 b8 40 00 00 41 88 94 07 c4 67 00 00 48 83 c0 01 48 83 f8 08 75 e6",
        0xd13c0: "41 8b 94 87 38 41 00 00 41 88 94 07 cc 67 00 00 48 83 c0 01 48 83 f8 08 75 e6",
        0xd13da: "83 fb 01 0f 86 95 08 00 00",
        # Class discriminator and the complete extension-0x204 gate/call. The
        # helper alone looks like a required default; it is D1B7-only, not CFB7.
        0xc7b68: "81 e2 00 00 08 00 b8 06 00 b7 d1 75 75",
        0xc7b8f: "81 e2 00 00 04 00 b8 06 00 b7 cf 75 4e",
        0xd1b41: "a9 00 00 08 00 74 1c",
        0xd1b48: "41 80 bf 58 0b 00 00 00 0f 84 a6 05 00 00",
        0xd1b56: "41 8b 87 80 12 00 00 41 89 87 00 69 00 00",
        0xd20fc: "49 8b 7f 38 49 8d b7 00 69 00 00 e8 d4 f1 04 00 e9 53 fa ff ff",
        0x1212e0: "8b 06 25 00 00 00 ff 83 c8 0f 89 06 c3",
        0x11e207: "8b 8a fc 01 00 00 8b b6 00 02 00 00",  # old timer <- new 0x200
        0x11e219: "81 e6 ff ff ff 7f",                     # low 31-bit timer
        0x11e227: "0f b6 b0 03 02 00 00",                  # high bit from new byte 0x203
        0x11e253: "48 8b b8 d4 01 00 00 48 89 ba d4 01 00 00", # common-prefix end
        0x11e26f: "81 c1 d8 01 00 00",                     # [4,0x1dc) prefix copy
        0x11e27b: "f3 0f 6f 80 e0 01 00 00 0f 11 82 dc 01 00 00", # half-scaled surface
        0x11e28a: "f3 0f 6f 88 f0 01 00 00",               # second half of same surface
        0xc9399: "41 b8 04 17 03 00 ba 0b 5a 16 00",        # timer min and max
        0xc93a4: "c1 e8 0a 69 c0 e1 00 00 00",              # (pixels >> 10) * 225
        0xddf00: "48 8d b3 fc 66 00 00 a9 00 00 04 00",     # CFB7 direct-record path
        0xddf44: "48 89 81 f8 02 00 00",                    # copy last 8 of 0x300
        0xddf51: "81 c1 00 03 00 00",                      # full upload size
        0xd18c3: "41 80 8f ff 68 00 00 80",                 # producer newer timer scale
        0xd0f96: "41 c6 87 1c 67 00 00 02",                 # reference block_height=2, tiled16=0
        0xd1095: "8b 4f 04 b8 01 00 00 00 d3 e0 83 e0 7f", # input 1 << allocation block exponent
        0xd10a2: "41 88 87 3c 67 00 00",                    # input flags at record 0x40
        0xd10c8: "41 c6 87 3c 67 00 00 80",                 # separate non-BL input branch
        0xd10df: "f3 41 0f 6f 8f 00 67 00 00",              # reference first 16-byte copy source
        0xd10e8: "f3 41 0f 6f 97 10 67 00 00",              # reference second half including flags
        0xd10ff: "41 0f 11 8f 40 67 00 00",                 # reconstructed descriptor first half
        0xd1115: "41 0f 11 97 50 67 00 00",                 # reconstructed second half with flags
    }
    for va, expected in anchors.items():
        expected = bytes.fromhex(expected)
        assert code(va, len(expected)) == expected, f"ABI evidence changed at {va:#x}"
    assert code(0x100a490, 8, executable=False) == struct.pack("<d", 256.0), "frame-rate scale changed"
    # Concrete encoder and base/derived backend tables, not a guessed slot.
    for va, target in ((0x1804238, 0x1275b0),
                       (0x17fe570, 0x2da00), (0x17ff008, 0x2da00),
                       (0x17ff120, 0x48ee0), (0x17fed68, 0x47a40)):
        assert code(va, 8, executable=False) == struct.pack('<Q', target), f"history fill vtable changed at {va:#x}"
    print(f"PASS {len(anchors)} audited instruction anchors in SHA-256 {SHA256}")
    # Independent Windows output reader. Read PE sections explicitly; never
    # load the DLL or execute an encoder to run this provenance check.
    win = (ROOT / 'out/nv610-windows-reference/nvcuvid.dll').read_bytes()
    assert hashlib.sha256(win).hexdigest() == '6014e9cbd0980fd55140056af52574142463431c9b1e232f14e7c929f7313686'
    pe = struct.unpack_from('<I', win, 0x3c)[0]
    assert win[pe:pe+4] == b'PE\0\0'
    nsects = struct.unpack_from('<H', win, pe+6)[0]
    optsize = struct.unpack_from('<H', win, pe+20)[0]
    opt = pe+24
    assert struct.unpack_from('<H', win, opt)[0] == 0x20b
    imagebase = struct.unpack_from('<Q', win, opt+24)[0]
    sections = []
    for i in range(nsects):
        sec = opt+optsize+40*i
        _, rva, size, offset = struct.unpack_from('<IIII', win, sec+8)
        sections.append((imagebase+rva, size, offset))
    windows_anchors = {
        0x1801036b0: '41 8b 6e 04 83 e5 03',
        0x180103722: '83 fd 02 75 0b',
        0x180103727: '41 8b 46 08 c1 e8 03 03 c8',
    }
    for va, expected in windows_anchors.items():
        expected = bytes.fromhex(expected)
        matches = [win[off+va-base:off+va-base+len(expected)]
                   for base, size, off in sections if base <= va and va+len(expected) <= base+size]
        assert matches == [expected], f'Windows H.264 completion evidence changed at {va:#x}'
    print('PASS 3 independent SHA-pinned Windows H.264 completion-state anchors')


def layout_check(compile_only=False):
    c = r'''
#include <stddef.h>
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern int puts(const char *);
extern int printf(const char *, ...);
extern void abort(void);
void __main(void) {}
#define check(x) ((x) ? (void)0 : (printf("line %d: %s\n",__LINE__,#x),abort()))
'''
    c += '#include "' + (ROOT / "kernel/nvenc_drv_h264.h").as_posix() + '"\n'
    c += r'''
_Static_assert(NVENC_CFB7_H264_STATUS_COMPLETE==2u,"driver-completed picture state");
int main(void) {
    static nvenc_h264_drv_pic_setup_s old;
    static nvenc_cfb7_h264_drv_pic_setup_s pic;
    unsigned char *wire=(unsigned char *)&pic;
    check(sizeof pic==768);
    check(NVENC_CFB7_DRV_MAGIC==0xcfb70006u);
    check(NVENC_CFB7_PICTURE_CODEC_H264==0u);
    memset(&pic,0,sizeof pic);
    pic.pic_control.codec=7;
    check(wire[0x1ae]==7);
    pic.pic_control.codec=NVENC_CFB7_PICTURE_CODEC_H264;
    check(wire[0x1ae]==0);
    check(NVENC_H264_TEST_WIDTH==256u && NVENC_H264_TEST_HEIGHT==256u);
    check(offsetof(nvenc_cfb7_h264_drv_pic_setup_s,input_cfg)==36);
    check(offsetof(nvenc_cfb7_h264_drv_pic_setup_s,outputpic_cfg)==68);
    check(offsetof(nvenc_cfb7_h264_drv_pic_setup_s,sps_data)==100);
    check(offsetof(nvenc_cfb7_h264_drv_pic_setup_s,pps_data)==104);
    check(offsetof(nvenc_cfb7_h264_drv_pic_setup_s,rate_control)==112);
    check(offsetof(nvenc_cfb7_h264_drv_pic_setup_s,rate_control)+
          offsetof(nvenc_h264_rc_s,framerate)==0x88);
    check(offsetof(nvenc_cfb7_h264_drv_pic_setup_s,pic_control)==200);
    nvenc_h264_me_control_s me={0};
    me.hint_type0=0;me.hint_type1=1;me.hint_type2=2;me.hint_type3=3;me.hint_type4=4;
    unsigned char *me_wire=(unsigned char *)&me;
    for(unsigned i=0;i<sizeof me;i++)
        check(me_wire[i]==(i==0x9a?0x88:i==0x9b?0x46:0));
    for(unsigned n=1;n<256;n++) {
        memset(&old,n,sizeof old);memset(&pic,n,sizeof pic);
        /* These adjacent records are precisely the prefix copied unchanged by
         * NVIDIA's backwards converter. Pattern checks catch holes or overlap. */
        check(!memcmp(&old.refpic_cfg,&pic.refpic_cfg,0x1d8));
        memset(&pic.half_scaled_outputpic_cfg,0x5a,sizeof pic.half_scaled_outputpic_cfg);
        pic.gpTimer_timeout_val=0x81234567;
        for(unsigned i=0x1e0;i<0x200;i++)check(wire[i]==0x5a);
        for(unsigned i=0x1dc;i<0x1e0;i++)check(wire[i]==n);
        for(unsigned i=0x204;i<0x300;i++)check(wire[i]==n);
        check(wire[0x200]==0x67&&wire[0x201]==0x45&&wire[0x202]==0x23&&wire[0x203]==0x81);
    }
    check(nvenc_cfb7_h264_timer(256,256)==0x80031704u);
    check(nvenc_cfb7_h264_timer(1920,1080)==(0x80000000u|455625u));
    check(nvenc_cfb7_h264_timer(2560,1440)==(0x80000000u|810000u));
    check(nvenc_cfb7_h264_timer(3840,2160)==0x80165a0bu);
    check(nvenc_cfb7_h264_timer(7680,4320)==0x80165a0bu);
    check(nvenc_cfb7_h264_timer(0xffffffffu,0xffffffffu)==0x80165a0bu);
    for(unsigned w=16;w<=8192;w+=16)for(unsigned h=16;h<=8192;h+=16) {
        unsigned ref=((w*h)>>10)*225;
        if(ref<202500)ref=202500;
        if(ref>1464843)ref=1464843;
        check(nvenc_cfb7_h264_timer(w,h)==(0x80000000u|ref));
    }
    puts("PASS CFB7 wire offsets, 255 sentinel layouts and 262144 dimension timer cases (host only)");
    return 0;
}
'''
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-cfb7-abi-") as tmp:
        src, obj, exe = (Path(tmp) / n for n in ("abi.c", "abi.obj", "abi.exe"))
        src.write_text(c)
        subprocess.run([clang, "--target=x86_64-w64-windows-gnu", "-mno-ms-bitfields",
                        "-ffreestanding", "-std=c11", "-O1", "-Wall", "-Wextra",
                        "-c", str(src), "-o", str(obj)], check=True)
        subprocess.run([clang, str(obj), "-o", str(exe)], check=True)
        if compile_only:
            print("COMPILE ONLY: CFB7 ABI harness linked; assertions NOT executed")
        else:
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-only', action='store_true',
                        help='inspect supplied binary and compile/link, never execute the harness')
    args = parser.parse_args()
    reference_check()
    layout_check(args.compile_only)
