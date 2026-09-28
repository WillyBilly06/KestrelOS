#!/usr/bin/env python3
"""Host tests of actual efuse reader/profile parser, never hardware access."""
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
import test_rtw89_firmware as fw


def main():
    header = fw.HEADER + '\n' + fw.SECURITY
    macros = '\n'.join(re.findall(r'^#define (?:R_BE_|B_BE_|MAC_AX_SYS_ACT)[^\n]+', header, re.M))
    code = fw.PREAMBLE.replace('static u32 rd32(volatile u8 *regs, u32 off) {(void)regs; (void)off; return fake_mmio;}', '')
    code += '\n' + macros + r'''
static u8 regs_store[0x40000];
static unsigned mode, polls, writes, commands, analog_writes;
static u16 analog_trace[8];
static u32 data_word;
static bool reachable(u32 off) {return off + 4 <= sizeof regs_store;}
static u32 rd32(volatile u8 *r, u32 off) {
    if (off == R_BE_IC_PWR_STATE) return mode == 1 ? 0 : MAC_AX_SYS_ACT << 16;
    if (off == R_BE_EFUSE_CTRL) {
        polls++;
        if (mode == 3) return 0xffffffffu;
        return mode == 2 ? 0x1580 : B_BE_EF_RDY | 0x1580;
    }
    if (off == R_BE_EFUSE_CTRL_1_V1) return data_word;
    u32 v; memcpy(&v, (const void *)(r + off), 4); return v;
}
static void wr32(volatile u8 *r, u32 off, u32 v) {
    if (off == R_BE_EFUSE_CTRL) {assert(v == 0x1580); commands++;}
    memcpy((void *)(r + off), &v, 4); writes++;
}
static u16 rd16(volatile u8 *r, u32 off) {u16 v; memcpy(&v, (const void *)(r + off), 2); return v;}
static void wr16(volatile u8 *r, u32 off, u16 v) {
    assert(off == R_BE_SYS_ISO_CTRL); assert(analog_writes<8);
    analog_trace[analog_writes++]=v; memcpy((void *)(r + off), &v, 2);
}
static void set8(volatile u8 *r, u32 off, u8 v) {r[off] |= v;}
static void timer_udelay(unsigned us) {(void)us;}
'''
    for name in ('rtw89_fw_security_decode', 'rtw89_efuse_read', 'rtw89_fw_security_read', 'rtw89_efuse_logical', 'rtw89_read_pci_mac'):
        code += fw.function(name) + '\n'
    code += r'''
static void run_read(unsigned failure, unsigned cut, u32 data, bool success) {
    memset(regs_store, 0, sizeof regs_store);
    u32 power = 0xa5081234, burst = 0x12345678;
    u16 iso = 0x1205;
    memcpy(regs_store + R_BE_WL_BT_PWR_CTRL, &power, 4);
    memcpy(regs_store + R_BE_EFUSE_CTRL_2_V1, &burst, 4);
    memcpy(regs_store + R_BE_SYS_ISO_CTRL, &iso, 2);
    regs_store[R_BE_PMC_DBG_CTRL2] = 0x21;
    mode = failure; data_word = data; polls = writes = commands = analog_writes = 0;
    rtw89_fw_security_t profile;
    memset(&profile, 0xff, sizeof profile);
    assert(rtw89_fw_security_read(regs_store, cut, &profile) == success);
    assert(profile.valid == success);
    assert(rd32(regs_store, R_BE_WL_BT_PWR_CTRL) == power);
    assert(rd32(regs_store, R_BE_EFUSE_CTRL_2_V1) == burst);
    assert(rd16(regs_store, R_BE_SYS_ISO_CTRL) == iso);
    assert(regs_store[R_BE_PMC_DBG_CTRL2] == 0x21);
    assert(commands == (failure == 1 ? 0 : 1));
    assert(analog_writes == (cut == 0 || failure == 1 ? 0 : 6));
    if(analog_writes) {
        const u16 expected[]={iso|B_BE_PWC_EV2EF_S,
            iso|B_BE_PWC_EV2EF_S|B_BE_PWC_EV2EF_B,
            (iso|B_BE_PWC_EV2EF_S|B_BE_PWC_EV2EF_B)&~B_BE_ISO_EB2CORE,
            iso|B_BE_PWC_EV2EF_S|B_BE_PWC_EV2EF_B|B_BE_ISO_EB2CORE,
            (iso|B_BE_PWC_EV2EF_S|B_BE_ISO_EB2CORE)&~B_BE_PWC_EV2EF_B,iso};
        assert(!memcmp(analog_trace,expected,sizeof expected));
    }
    if (failure == 2) assert(polls == 1000000);
    if (failure == 3) assert(polls == 1);
}
int main(void) {
    rtw89_fw_security_t p;
    u8 bytes[4] = {0xfc, 0x4f, 0xff, 0xff};
    assert(rtw89_fw_security_decode(bytes, &p) && !p.secure_boot);
    unsigned accepted = 0;
    for (unsigned s = 0; s <= 0xffff; ++s) {
        bytes[2] = s; bytes[3] = s >> 8;
        bool ok = rtw89_fw_security_decode(bytes, &p);
        if (ok && p.secure_boot) {
            accepted++;
            assert(s == 0xf07e || s == 0xf0ff || s == 0xe17e || s == 0xe1ff);
            assert(p.device_type == 0 && p.customer == 0 && p.key == 0);
            assert(p.selector == ((s >> 8 & 15) ? 0x0c000180u : 0x08000100u));
        } else assert(!ok || s == 0xffff);
    }
    assert(accepted == 4);
    const u8 types[5] = {0xc, 0xa, 9, 6, 15};
    for (unsigned t=0; t<5; ++t) for(unsigned c=0;c<32;++c) for(unsigned k=0;k<16;++k) {
        unsigned invc = 31-c;
        bytes[0] = types[t] | (invc & 15) << 4;
        bytes[1] = ((invc >> 4) << 6) | (15-k);
        bytes[2] = 0x7e; bytes[3] = 0xf0;
        assert(rtw89_fw_security_decode(bytes, &p));
        assert(p.customer == c && p.key == k && p.device_type == (t == 4 ? 15 : t));
    }
    bytes[0] = 0;
    assert(!rtw89_fw_security_decode(bytes, &p) && !p.valid);
    assert(!rtw89_fw_security_decode(NULL, &p) && !p.valid);
    assert(!rtw89_fw_security_read(NULL, 1, &p) && !p.valid);
    for(unsigned cut=0;cut<2;++cut) {
        run_read(0,cut,0xffffffffu,true); /* genuinely unprogrammed efuse is valid */
        run_read(0,cut,0xf07e4ffcu,true);
        run_read(0,cut,0x00000000u,false);
        run_read(1,cut,0xffffffffu,false);
        run_read(2,cut,0xffffffffu,false);
        run_read(3,cut,0xffffffffu,false); /* absent MMIO is never efuse FF */
    }
    u8 map[20], logical[8], mac[6];
    memset(map,0xff,sizeof map);
    map[4]=0x32; map[5]=0; map[6]=0x0c; /* page1, block0, words0/1 */
    map[7]=1; map[8]=2; map[9]=3; map[10]=4;
    map[11]=0x32; map[12]=0; map[13]=0x0e; /* later word0 override */
    map[14]=5; map[15]=6;
    assert(rtw89_efuse_logical(map,sizeof map,1,0,logical,8));
    assert(logical[0]==5 && logical[1]==6 && logical[2]==3 && logical[3]==4 && logical[4]==0xff);
    assert(rtw89_efuse_logical(map,sizeof map,1,1,logical,3));
    assert(logical[0]==6 && logical[1]==3 && logical[2]==4);
    assert(rtw89_efuse_logical(map,sizeof map,2,0,logical,8) && logical[0]==0xff);
    for(unsigned n=5;n<16;++n)
        if(n!=11) assert(!rtw89_efuse_logical(map,n,1,0,logical,8));
    assert(!rtw89_efuse_logical(map,sizeof map,7,0,logical,8));
    assert(!rtw89_efuse_logical(map,sizeof map,1,0xffff,logical,8));
    memset(regs_store+0x3104,0,6);
    assert(!rtw89_read_pci_mac(regs_store,mac));
    const u8 validmac[6]={0x10,0x22,0x33,0x44,0x55,0x66};
    memcpy(regs_store+0x3104,validmac,6);
    assert(rtw89_read_pci_mac(regs_store,mac) && !memcmp(mac,validmac,6));
    regs_store[0x3104]|=1;
    assert(!rtw89_read_pci_mac(regs_store,mac));
    puts("PASS: 65536 selectors; all customer/key profiles; cut A/B read-only efuse transaction; timeout/absent MMIO restore");
}
'''
    with tempfile.TemporaryDirectory(prefix='kestrel-rtw89-security-') as temp:
        src = Path(temp) / 'test.c'
        exe = Path(temp) / 'test.exe'
        src.write_text(code)
        cc = shutil.which('clang') or r'C:\Program Files\LLVM\bin\clang.exe'
        subprocess.run([cc, '-std=c11', '-O2', '-Wall', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
