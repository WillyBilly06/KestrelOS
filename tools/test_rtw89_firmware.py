#!/usr/bin/env python3
"""Execute the actual rtw89 MFW selector, v1 parser and downloader on host.

This compiles the function bodies from kernel/rtw89.c into a temporary host
executable.  Both bundled RTL8922 firmware packages are tested at cut 0 and cut
1, including byte-for-byte reconstruction of exactly what the downloader sends.
No hardware or USB device is accessed.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "kernel/rtw89.c").read_text()
SECURITY = (ROOT / "kernel/rtw89_security.h").read_text()
HEADER = (ROOT / "kernel/rtw89.h").read_text()
BRINGUP = (ROOT / "kernel/rtw.c").read_text()


def function(name):
    source = SECURITY if name.startswith(("rtw89_fw_security_", "rtw89_efuse_", "rtw89_read_pci_mac")) or name == "rtw89_fw_apply_security" else SOURCE
    m = re.search(r"^(?:static )?(?:bool|u8|u16|u32|void) " + name +
                  r"\([^;]*?\)\s*\{", source, re.M)
    if not m:
        raise AssertionError("function not found: " + name)
    end = source.index("\n}", m.end()) + 2
    return source[m.start():end]


def macro(name):
    m = re.search(r"^#define\s+" + re.escape(name) + r"\s+[^\n]+", HEADER, re.M)
    if not m:
        raise AssertionError("macro not found: " + name)
    return m.group(0)


MACROS = (
    "R_AX_SYS_CFG1", "B_AX_CHIP_VER_MASK", "B_AX_CHIP_VER_SHIFT",
    "RTW89_MFW_SIG", "RTW89_MFW_HEADER_BYTES", "RTW89_MFW_ENTRY_BYTES",
    "RTW89_FW_NORMAL", "RTW89_FW_HDR_V1_WORDS", "RTW89_FW_HDR_V1_BYTES",
    "RTW89_FW_SECTION_V1_BYTES", "RTW89_FW_MAX_SECTIONS",
    "FWDL_SECTION_CHKSUM_LEN", "FWDL_SECURITY_SECTION_TYPE",
    "FWDL_SECURITY_SIGLEN", "FWDL_SECURITY_CHKSUM_LEN",
    "FW_HDR_V1_W1_MAJOR_SHIFT", "FW_HDR_V1_W1_MINOR_SHIFT",
    "FW_HDR_V1_W1_SUBVER_SHIFT", "FW_HDR_V1_W1_SUBIDX_SHIFT",
    "FW_HDR_V1_W3_HDR_VER_SHIFT", "FW_HDR_V1_W4_MONTH_SHIFT",
    "FW_HDR_V1_W4_DATE_SHIFT", "FW_HDR_V1_W5_YEAR_MASK",
    "FW_HDR_V1_W5_HDR_SIZE_SHIFT", "FW_HDR_V1_W6_SEC_NUM_SHIFT",
    "FW_HDR_V1_W6_DSP_CHKSUM", "FW_HDR_V1_W7_DYN_HDR",
    "FWSEC_V1_W1_SIZE_MASK", "FWSEC_V1_W1_TYPE_SHIFT",
    "FWSEC_V1_W1_TYPE_MASK", "FWSEC_V1_W1_CHECKSUM", "FWSEC_V1_W1_REDL",
    "FWDL_SECTION_PER_PKT_LEN", "H2C_HEADER_LEN", "H2C_HDR_CAT_SHIFT",
    "H2C_HDR_CAT_MASK", "H2C_HDR_CLASS_SHIFT", "H2C_HDR_CLASS_MASK",
    "H2C_HDR_FUNC_SHIFT", "H2C_HDR_FUNC_MASK", "H2C_HDR_DEL_TYPE_SHIFT",
    "H2C_HDR_DEL_TYPE_MASK", "H2C_HDR_SEQ_SHIFT", "H2C_HDR_SEQ_MASK",
    "H2C_HDR_TOTAL_LEN_MASK", "H2C_HDR_REC_ACK", "H2C_HDR_DONE_ACK",
    "H2C_CAT_MAC", "H2C_CL_MAC_FWDL", "H2C_FUNC_MAC_FWHDR_DL",
)


PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define kwarn(...) ((void)0)
#define kerr(...) ((void)0)
#define kinfo(...) ((void)0)
static bool quiet_refusals;
typedef struct {
    u32 download_address, length, mssc_length;
    u32 key_offset, key_length;
    u8 type, mssc;
    bool redownload;
} rtw89_fw_section_t;
typedef struct {
    u8 major, minor, subversion, subindex;
    u16 year;
    u8 month, date, header_version;
    u32 header_length, dynamic_header_length, part_size;
    int section_count;
    rtw89_fw_section_t sections[16];
    u32 payload_bytes, file_bytes;
    bool needs_security_profile;
    bool security_validated;
} rtw89_fw_info_t;
typedef struct {
    bool valid, secure_boot;
    u8 device_type, customer, key;
    u32 selector;
} rtw89_fw_security_t;
typedef struct {
    const u8 *data;
    size_t size;
    u8 cv, type;
    bool from_container;
} rtw89_fw_image_t;
typedef bool (*rtw89_h2c_send_fn)(void *, const u8 *, u32, bool);
static u32 fake_mmio;
static u32 rd32(volatile u8 *regs, u32 off) {(void)regs; (void)off; return fake_mmio;}
'''


TEST_BODY = r'''
typedef struct {u8 *bytes; size_t used, cap; unsigned packets;} capture_t;

static u32 host_le32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static bool capture(void *opaque, const u8 *packet, u32 len, bool fwdl) {
    capture_t *c = opaque;
    size_t prefix = fwdl ? 0 : H2C_HEADER_LEN;
    assert(fwdl == (c->packets != 0));
    assert(len > prefix && len <= prefix + FWDL_SECTION_PER_PKT_LEN);
    if (!fwdl) {
        u32 w0 = host_le32(packet), w1 = host_le32(packet + 4);
        assert(((w0 >> H2C_HDR_CAT_SHIFT) & H2C_HDR_CAT_MASK) == H2C_CAT_MAC);
        assert(((w0 >> H2C_HDR_CLASS_SHIFT) & H2C_HDR_CLASS_MASK) == H2C_CL_MAC_FWDL);
        assert(((w0 >> H2C_HDR_FUNC_SHIFT) & H2C_HDR_FUNC_MASK) == H2C_FUNC_MAC_FWHDR_DL);
        assert((w1 & H2C_HDR_TOTAL_LEN_MASK) == len);
        assert(!(w1 & (H2C_HDR_REC_ACK | H2C_HDR_DONE_ACK)));
    }
    size_t n = len - prefix;
    assert(c->used + n <= c->cap);
    memcpy(c->bytes + c->used, packet + prefix, n);
    c->used += n;
    c->packets++;
    return true;
}

static u8 *read_file(const char *name, size_t *size) {
    FILE *f = fopen(name, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n > 0 && fseek(f, 0, SEEK_SET) == 0);
    u8 *p = malloc((size_t)n);
    assert(p && fread(p, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (size_t)n;
    return p;
}

static void exercise_image(const u8 *whole, size_t whole_size, u8 cv,
                           bool expect_security_profile, bool secure) {
    rtw89_fw_image_t chosen;
    assert(rtw89_select_firmware(whole, whole_size, cv, RTW89_FW_NORMAL, &chosen));
    assert(chosen.from_container && chosen.cv == cv && chosen.type == RTW89_FW_NORMAL);
    assert(chosen.data >= whole && chosen.size <= whole_size);
    assert((size_t)(chosen.data - whole) <= whole_size - chosen.size);

    rtw89_fw_info_t info;
    assert(rtw89_parse_firmware(chosen.data, chosen.size, &info));
    assert(info.header_version == 1 && info.section_count >= 3 && info.section_count <= 4);
    assert(info.part_size > 0 && info.part_size <= FWDL_SECTION_PER_PKT_LEN);
    assert(info.header_length <= chosen.size);
    assert(info.file_bytes + info.header_length == chosen.size);
    assert(info.needs_security_profile == expect_security_profile);

    u8 *sent = malloc(chosen.size);
    u8 *expected = malloc(chosen.size);
    assert(sent && expected);
    capture_t c = {.bytes = sent, .cap = chosen.size};

    if (expect_security_profile) {
        assert(!rtw89_fw_download(chosen.data, chosen.size, &info, capture, &c));
        assert(c.packets == 0); /* fail closed before any hardware hand-over */
    }

    rtw89_fw_security_t profile;
    const u8 unsecure[4] = {0xff, 0xff, 0xff, 0xff};
    const u8 secured[4] = {0xfc, 0x4f, 0x7e, 0xf0};
    assert(rtw89_fw_security_decode(secure ? secured : unsecure, &profile));
    assert(profile.valid && profile.secure_boot == secure);
    assert(rtw89_fw_apply_security(chosen.data, chosen.size, &profile, &info));

    assert(rtw89_fw_download(chosen.data, chosen.size, &info, capture, &c));

    size_t fixed = info.header_length - info.dynamic_header_length;
    size_t used = 0, at = info.header_length;
    memcpy(expected, chosen.data, fixed);
    expected[28] = FWDL_SECTION_PER_PKT_LEN & 0xff;
    expected[29] = FWDL_SECTION_PER_PKT_LEN >> 8;
    used += fixed;
    for (int i = 0; i < info.section_count; i++) {
        assert(at + info.sections[i].length + info.sections[i].mssc_length <= chosen.size);
        memcpy(expected + used, chosen.data + at, info.sections[i].length);
        if (info.sections[i].key_length)
            memcpy(expected + used + info.sections[i].length - info.sections[i].key_length,
                   chosen.data + info.sections[i].key_offset, info.sections[i].key_length);
        used += info.sections[i].length;
        at += info.sections[i].length + info.sections[i].mssc_length;
    }
    assert(at == chosen.size && used == fixed + info.payload_bytes);
    assert(c.used == used && c.packets > 1 && memcmp(sent, expected, used) == 0);

    rtw89_fw_info_t valid = info;
    assert(!rtw89_parse_firmware(chosen.data, chosen.size - 1, &info));
    unsigned packets_before = c.packets;
    assert(!rtw89_fw_download(chosen.data, chosen.size - 1, &valid, capture, &c));
    assert(c.packets == packets_before); /* fail before a partial hand-over */

    valid.part_size = 0;
    capture_t no_partial = {.bytes = sent, .cap = chosen.size};
    assert(!rtw89_fw_download(chosen.data, chosen.size, &valid, capture, &no_partial));
    assert(no_partial.packets == 0);
    free(expected);
    free(sent);
}

static void exercise_bb0(const u8 *whole, size_t whole_size, u8 cv) {
    const u8 *bb = NULL;
    size_t size = 0;
    assert(rtw89_fw_element_select(whole, whole_size, cv, 0, &bb, &size));
    assert(size == 33560);
    rtw89_fw_info_t info;
    assert(rtw89_parse_firmware(bb, size, &info));
    assert(info.header_length == 80 && info.section_count == 2);
    assert(info.sections[0].type == 10 && info.sections[0].length == 32768);
    assert(info.sections[1].type == 9 && info.sections[1].length == 712);
    assert(!info.sections[0].mssc_length && !info.sections[1].mssc_length);
    assert(!info.needs_security_profile);
    u8 *sent = malloc(size), *expected = malloc(size);
    assert(sent && expected);
    capture_t c = {.bytes = sent, .cap = size};
    assert(rtw89_fw_download(bb, size, &info, capture, &c));
    size_t fixed = info.header_length - info.dynamic_header_length;
    memcpy(expected, bb, fixed);
    expected[28] = FWDL_SECTION_PER_PKT_LEN & 0xff;
    expected[29] = FWDL_SECTION_PER_PKT_LEN >> 8;
    size_t at = info.header_length, used = fixed;
    for (int i = 0; i < info.section_count; i++) {
        memcpy(expected + used, bb + at, info.sections[i].length);
        at += info.sections[i].length;
        used += info.sections[i].length;
    }
    assert(at == size && used == c.used && !memcmp(sent, expected, used));
    assert(c.packets > 16); /* real BB MCU spans many CH12 descriptors */
    free(expected);
    free(sent);
}

static void failure_cases(const u8 *whole, size_t size) {
    rtw89_fw_image_t chosen;
    assert(!rtw89_select_firmware(whole, 15, 0, RTW89_FW_NORMAL, &chosen));
    u8 *bad = malloc(size);
    assert(bad);
    memcpy(bad, whole, size);
    bad[1] = 0;
    assert(!rtw89_select_firmware(bad, size, 0, RTW89_FW_NORMAL, &chosen));
    memcpy(bad, whole, size);
    /* Cut-1 normal entry is the second 16-byte record in these packages. */
    bad[16 + 16 + 8] = 0xff;
    bad[16 + 16 + 9] = 0xff;
    bad[16 + 16 + 10] = 0xff;
    bad[16 + 16 + 11] = 0x7f;
    assert(!rtw89_select_firmware(bad, size, 1, RTW89_FW_NORMAL, &chosen));
    assert(!rtw89_select_firmware(whole, size, 1, 2, &chosen));
    free(bad);
}

static void signature_cases(void) {
    enum { FIXED=64, SECTION=1024, POOL=FIXED+SECTION, SIZE=POOL+33+512 };
    u8 blob[SIZE], sent[SIZE];
    memset(blob,0,sizeof blob);
    put32(blob+12,1u<<FW_HDR_V1_W3_HDR_VER_SHIFT);
    put32(blob+24,1u<<FW_HDR_V1_W6_SEC_NUM_SHIFT);
    put32(blob+28,FWDL_SECTION_PER_PKT_LEN);
    put32(blob+48,0x30000);
    put32(blob+52,SECTION | FWDL_SECURITY_SECTION_TYPE<<FWSEC_V1_W1_TYPE_SHIFT);
    put32(blob+56,0xff);
    memcpy(blob+POOL,"MSSKPOOL",8);
    put32(blob+POOL+8,32);
    put32(blob+POOL+12,33);
    blob[POOL+21]=1; blob[POOL+22]=1;
    blob[POOL+24]=1; blob[POOL+26]=8;
    blob[POOL+32]=1; /* device 0/customer 0/key 0 */
    memset(blob+POOL+33,0xa5,512);
    u8 secbytes[4]={0xfc,0x4f,0x7e,0xf0};
    rtw89_fw_security_t profile;
    rtw89_fw_info_t info;
    assert(rtw89_fw_security_decode(secbytes,&profile) && profile.secure_boot);
    assert(rtw89_fw_apply_security(blob,sizeof blob,&profile,&info));
    assert(info.security_validated && info.sections[0].key_offset==POOL+33);
    assert(info.sections[0].key_length==512);
    capture_t c={.bytes=sent,.cap=sizeof sent};
    assert(rtw89_fw_download(blob,sizeof blob,&info,capture,&c));
    assert(c.used==FIXED+SECTION);
    assert(!memcmp(sent+FIXED,blob+FIXED,SECTION-512));
    assert(!memcmp(sent+FIXED+SECTION-512,blob+POOL+33,512));
    assert(blob[FIXED+SECTION-1]==0); /* firmware file remains immutable */
    profile.valid=false;
    assert(!rtw89_fw_apply_security(blob,sizeof blob,&profile,&info) && !info.security_validated);
    profile.valid=true; profile.key=1;
    assert(!rtw89_fw_apply_security(blob,sizeof blob,&profile,&info));
    profile.key=0;
    put32(blob+FIXED+58,0x08000101u); /* selector mismatch, low bits don't alter key length */
    assert(!rtw89_fw_apply_security(blob,sizeof blob,&profile,&info));
    put32(blob+FIXED+58,0);
    blob[POOL+22]=0; /* malformed key count must not index beyond pool */
    assert(!rtw89_fw_apply_security(blob,sizeof blob,&profile,&info));
    blob[POOL+22]=1;
    blob[POOL+32]=0;
    assert(!rtw89_fw_apply_security(blob,sizeof blob,&profile,&info));
}

int main(int argc, char **argv) {
    assert(argc == 3);
    u8 cv = 0xff;
    fake_mmio = 0xffffffffu;
    assert(!rtw89_chip_cv((volatile u8 *)1, &cv) && cv == 0xff);
    fake_mmio = 0x00001000u;
    assert(rtw89_chip_cv((volatile u8 *)1, &cv) && cv == 1);
    assert(!rtw89_chip_cv(NULL, &cv) && !rtw89_chip_cv((volatile u8 *)1, NULL));

    signature_cases();
    for (int i = 1; i < argc; i++) {
        size_t size;
        u8 *whole = read_file(argv[i], &size);
        assert(whole[0] == RTW89_MFW_SIG && whole[1] == 5);
        exercise_image(whole, size, 0, false, false);
        exercise_image(whole, size, 1, i == 1, false);
        if (i == 1) exercise_image(whole, size, 1, true, true);
        exercise_bb0(whole, size, 0);
        exercise_bb0(whole, size, 1);
        failure_cases(whole, size);
        free(whole);
    }
    puts("PASS: actual rtw89 normal+BBMCU0 parser/downloader; 2 MFW files x CV0/CV1; MSSC admission and byte-exact image transfer; malformed bounds; invalid MMIO");
    return 0;
}
'''


def main():
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    code = PREAMBLE + '#include "rtw89_radio_tables.h"\n' + "\n".join(macro(n) for n in MACROS) + "\n"
    for name in ("le32_at", "le16_at", "put32", "rtw89_chip_cv",
                 "rtw89_select_firmware", "rtw89_formatted_mssc_length",
                 "rtw89_parse_firmware", "rtw89_h2c_header",
                 "rtw89_fw_security_decode", "rtw89_fw_apply_security",
                 "rtw89_fw_download"):
        code += "\n" + function(name) + "\n"
    code += TEST_BODY

    with tempfile.TemporaryDirectory(prefix="kestrel-rtw89-host-") as tmp:
        cfile = Path(tmp) / "rtw89_firmware_test.c"
        exe = Path(tmp) / "rtw89_firmware_test.exe"
        cfile.write_text(code)
        subprocess.run([clang, "-D_CRT_SECURE_NO_WARNINGS", "-std=c11", "-O2", "-Wall", "-Wextra",
                        "-I", str(ROOT / "kernel"),
                        str(cfile), "-o", str(exe)], check=True)
        subprocess.run([str(exe),
                        str(ROOT / "firmware/rtw89/rtw8922a_fw.bin"),
                        str(ROOT / "firmware/rtw89/rtw8922a_fw-1.bin")], check=True)

    # Keep the production guard between parsing and every state-changing step.
    # The downloader has its own guard, but this prevents starting the CPU or
    # resetting DMA for an image that cannot be authenticated correctly.
    parse_at = BRINGUP.index("rtw89_parse_firmware(selected.data")
    gate_at = BRINGUP.index("rtw89_fw_security_read(", parse_at)
    selection_at = BRINGUP.index("rtw89_fw_apply_security(", gate_at)
    dma_at = BRINGUP.index("rtw89_dma_reset(", gate_at)
    cpu_at = BRINGUP.index("rtw89_fwdl_start_cpu(", gate_at)
    download_at = BRINGUP.index("be_download_firmware(regs", gate_at)
    assert parse_at < gate_at < selection_at < dma_at < cpu_at < download_at


if __name__ == "__main__":
    main()
