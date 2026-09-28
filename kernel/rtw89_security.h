/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Register/profile algorithms adapted from Realtek's Linux rtw89 driver:
 * Copyright(c) 2023 Realtek Corporation.
 * RTL8922A firmware security admission. Register sequence and file fields:
 * Linux v6.17 drivers/net/wireless/realtek/rtw89/{efuse_be.c,efuse.c,fw.c,reg.h}.
 * Reads efuse only: no efuse programming command is issued. Unknown profiles,
 * inaccessible MMIO and absent signing keys fail before firmware/DMA startup.
 */
#ifndef KESTREL_RTW89_SECURITY_H
#define KESTREL_RTW89_SECURITY_H

#define R_BE_EFUSE_CTRL               0x0030
#define R_BE_EFUSE_CTRL_1_V1          0x0034
#define R_BE_EFUSE_CTRL_2_V1          0x00A4
#define B_BE_EF_RDY                  (1u << 29)
#define B_BE_EF_BURST                (1u << 19)
#define R_BE_WL_BT_PWR_CTRL           0x0068
#define B_BE_BT_DISN_EN               (1u << 16)
#define R_BE_IC_PWR_STATE             0x03F0
#define B_BE_WHOLE_SYS_PWR_STE_MASK   0x03FF0000u
#define MAC_AX_SYS_ACT               0x220u

bool rtw89_fw_security_decode(const u8 bytes[4], rtw89_fw_security_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!bytes) return false;
    u16 selector = (u16)bytes[2] | (u16)bytes[3] << 8;
    if (selector == 0xFFFFu) {
        out->valid = true;
        return true; /* Linux's explicit non-secure efuse encoding. */
    }
    unsigned base = 0, zeros = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned lo = (bytes[2] >> i) & 1u;
        if (lo != ((bytes[2] >> (7 - i)) & 1u)) return false;
        if (!lo) { base = i * 16; if (++zeros > 1) return false; }
    }
    if (((bytes[3] & 15u) ^ (bytes[3] >> 4)) != 15u) return false;
    unsigned idx = base + (bytes[3] & 15u);
    if (idx > 1) return false;
    switch (bytes[0] & 15u) {
    case 0xC: out->device_type = 0; break;
    case 0xA: out->device_type = 1; break;
    case 0x9: out->device_type = 2; break;
    case 0x6: out->device_type = 3; break;
    case 0xF: out->device_type = 15; break;
    default: return false;
    }
    out->customer = (u8)(31u - ((bytes[0] >> 4) | ((bytes[1] >> 6) & 1u) << 4));
    out->key = (u8)(15u - (bytes[1] & 15u));
    out->selector = idx ? 0x0C000180u : 0x08000100u;
    out->secure_boot = out->valid = true;
    return true;
}

bool rtw89_efuse_read(volatile u8 *regs, u8 cv, u32 address, u8 *bytes, u32 length) {
    if (!regs || !bytes || !length || (address & 3u) || (length & 3u) ||
        address > 0x2000u || length > 0x2000u - address ||
        !reachable(R_BE_IC_PWR_STATE)) return false;
    memset(bytes, 0, length);
    u32 power = rd32(regs, R_BE_WL_BT_PWR_CTRL);
    u32 burst = rd32(regs, R_BE_EFUSE_CTRL_2_V1);
    u16 iso = rd16(regs, R_BE_SYS_ISO_CTRL);
    u8 debug = *(volatile u8 *)(regs + R_BE_PMC_DBG_CTRL2);
    if (power == 0xFFFFFFFFu || burst == 0xFFFFFFFFu || iso == 0xFFFFu ||
        debug == 0xFFu) return false;
    bool active = false, ready = false;
    wr32(regs, R_BE_WL_BT_PWR_CTRL, power & ~B_BE_BT_DISN_EN);
    for (unsigned i = 0; i < 100; ++i) {
        u32 state = rd32(regs, R_BE_IC_PWR_STATE);
        if (state == 0xFFFFFFFFu) break;
        if (((state & B_BE_WHOLE_SYS_PWR_STE_MASK) >> 16) == MAC_AX_SYS_ACT) {
            active = true; break;
        }
        timer_udelay(50);
    }
    if (!active) goto restore_power;
    set8(regs, R_BE_PMC_DBG_CTRL2, B_BE_SYSON_DIS_PMCR_BE_WRMSK);
    /* Cut A has no analog-power workaround; subsequent cuts require it. */
    if (cv != 0) {
        wr16(regs, R_BE_SYS_ISO_CTRL, iso | B_BE_PWC_EV2EF_S);
        timer_udelay(1000);
        wr16(regs, R_BE_SYS_ISO_CTRL, iso | B_BE_PWC_EV2EF_S | B_BE_PWC_EV2EF_B);
        wr16(regs, R_BE_SYS_ISO_CTRL,
             (iso | B_BE_PWC_EV2EF_S | B_BE_PWC_EV2EF_B) & ~B_BE_ISO_EB2CORE);
    }
    wr32(regs, R_BE_EFUSE_CTRL_2_V1, burst | B_BE_EF_BURST);
    for (u32 pos = 0; pos < length; pos += 4) {
        /* Aligned physical address only. RDY/WRITE bits zero: read only. */
        wr32(regs, R_BE_EFUSE_CTRL, address + pos);
        ready = false;
        for (unsigned i = 0; i < 1000000; ++i) {
            timer_udelay(1);
            u32 control = rd32(regs, R_BE_EFUSE_CTRL);
            if (control == 0xFFFFFFFFu) break;
            if (control & B_BE_EF_RDY) {
                u32 data = rd32(regs, R_BE_EFUSE_CTRL_1_V1);
                for (unsigned j = 0; j < 4; ++j) bytes[pos+j] = (u8)(data >> (j*8));
                ready = true; break;
            }
        }
        if (!ready) break;
    }
    /* Reverse the power sequence on success AND timeout. Preserve unrelated
     * state, including a pre-existing power state, instead of leaking it. */
    if (cv != 0) {
        wr16(regs, R_BE_SYS_ISO_CTRL,
             rd16(regs, R_BE_SYS_ISO_CTRL) | B_BE_ISO_EB2CORE);
        wr16(regs, R_BE_SYS_ISO_CTRL,
             (rd16(regs, R_BE_SYS_ISO_CTRL) | B_BE_ISO_EB2CORE) & ~B_BE_PWC_EV2EF_B);
        timer_udelay(1000);
        wr16(regs, R_BE_SYS_ISO_CTRL, iso);
    }
    *(volatile u8 *)(regs + R_BE_PMC_DBG_CTRL2) = debug;
    wr32(regs, R_BE_EFUSE_CTRL_2_V1, burst);
restore_power:
    wr32(regs, R_BE_WL_BT_PWR_CTRL, power);
    if (!ready) memset(bytes, 0, length);
    return ready;
}

bool rtw89_fw_security_read(volatile u8 *regs, u8 cv,
                           rtw89_fw_security_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    u8 bytes[4];
    if (!rtw89_efuse_read(regs, cv, 0x1580, bytes, sizeof bytes) ||
        !rtw89_fw_security_decode(bytes, out)) return false;
    kinfo("rtw89", "firmware security profile: secure=%u device=%u customer=%u key=%u",
          out->secure_boot, out->device_type, out->customer, out->key);
    return true;
}

/* DDV logical blocks use a three-byte big-endian record header, then each
 * present little-endian word; later records override earlier words. Four
 * physical security-control bytes precede the records (RTL8922A chip info). */
bool rtw89_efuse_logical(const u8 *physical, size_t length, u8 page,
                        u32 offset, u8 *logical, u32 count) {
    if (!physical || length < 4 || !logical || !count || page > 6 ||
        offset > 0x10000u || count > 0x10000u-offset) return false;
    memset(logical, 0xff, count);
    size_t p = 4;
    while (p < length) {
        if (physical[p] == 0xff) return true;
        if (length-p < 3) return false;
        if (physical[p+1] == 0xff || physical[p+2] == 0xff) return true;
        u32 header = (u32)physical[p]<<16 | (u32)physical[p+1]<<8 | physical[p+2];
        p += 3;
        u32 record_page = (header>>17)&7u;
        u32 block = (header>>4)&0x1fffu;
        for (unsigned word=0; word<4; ++word) {
            if (header & (1u<<word)) continue;
            if (length-p < 2) return false;
            u32 logical_at = block*8+word*2;
            for (unsigned b=0;b<2;++b)
                if (record_page==page && logical_at+b>=offset && logical_at+b-offset<count)
                    logical[logical_at+b-offset] = physical[p+b];
            p+=2;
        }
    }
    return true;
}

bool rtw89_read_pci_mac(volatile u8 *regs, u8 mac[6]) {
    if (!regs || !mac || !reachable(0x3108)) return false;
    u8 candidate[6];
    for (unsigned i=0;i<6;i+=2) {
        u16 v = rd16(regs, 0x3104+i);
        candidate[i]=(u8)v; candidate[i+1]=(u8)(v>>8);
    }
    if (candidate[0]&1u) return false;
    unsigned any=0;
    for (unsigned i=0;i<6;++i) any |= candidate[i];
    if (!any) return false;
    memcpy(mac,candidate,6);
    return true;
}

bool rtw89_fw_apply_security(const u8 *fw, size_t size,
                            const rtw89_fw_security_t *profile,
                            rtw89_fw_info_t *info) {
    if (!info) return false;
    info->security_validated = false;
    if (!fw || !profile || !profile->valid) return false;
    /* Never trust a stale caller's section offsets or lengths. */
    rtw89_fw_info_t checked;
    if (!rtw89_parse_firmware(fw, size, &checked)) return false;
    size_t at = checked.header_length;
    unsigned secure_sections = 0;
    for (int i = 0; i < checked.section_count; ++i) {
        rtw89_fw_section_t *s = &checked.sections[i];
        if (s->type == FWDL_SECURITY_SECTION_TYPE) {
            if (++secure_sections > 1) return false; /* multi-signature-section selection not implemented */
            if (profile->secure_boot) {
                if (!s->mssc_length || s->length < FWDL_SECURITY_SIGLEN) return false;
                u32 key_len = FWDL_SECURITY_SIGLEN, key_at = s->length;
                if (s->mssc == 0xFFu) {
                    const u8 *pool = fw + at + s->length;
                    u32 raw = le32_at(pool + 12);
                    u32 selector = le32_at(fw + at + 58);
                    if (selector && selector != profile->selector) return false;
                    u32 keys = le16_at(pool + 26), customers = le16_at(pool + 24);
                    if (!keys || profile->key >= keys || !customers || profile->customer >= customers)
                        return false;
                    u64 bit = (u64)profile->customer * keys + profile->key;
                    if (profile->device_type == 15) {
                        if (!pool[16] || bit >= 64) return false;
                    } else {
                        if (profile->device_type >= pool[21]) return false;
                        bit += (u64)profile->device_type * keys * customers + (pool[16] ? 64 : 0);
                    }
                    if (raw < 32 || bit / 8 >= raw - 32) return false;
                    const u8 *map = pool + 32;
                    if (!(map[bit / 8] & (1u << (bit & 7)))) return false;
                    u32 idx = 0;
                    for (u64 b = 0; b < bit; ++b) idx += (map[b / 8] >> (b & 7)) & 1u;
                    if (idx >= le16_at(pool + 22)) return false;
                    key_len = le16_at(fw + at + 60) >> 2;
                    if (!key_len) key_len = FWDL_SECURITY_SIGLEN;
                    if (le32_at(fw + 24) & FW_HDR_V1_W6_DSP_CHKSUM)
                        key_len += FWDL_SECURITY_CHKSUM_LEN;
                    if ((u64)raw + (u64)idx * key_len + key_len > s->mssc_length) return false;
                    key_at += raw + idx * key_len;
                }
                /* Linux BE uses the zero-initialized legacy MSS index; only
                 * the AX v0 recognition path sets a legacy index. */
                if (key_len > s->length || key_at < s->length ||
                    (u64)key_at + key_len > (u64)s->length + s->mssc_length) return false;
                s->key_offset = (u32)at + key_at;
                s->key_length = key_len;
            }
        }
        at += (size_t)s->length + s->mssc_length;
    }
    if (profile->secure_boot && !secure_sections) return false;
    checked.security_validated = true;
    *info = checked;
    return true;
}
#endif
