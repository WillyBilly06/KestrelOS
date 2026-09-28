/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Resource formats and selection adapted from Realtek's Linux rtw89 driver,
 * Copyright(c) 2019-2025 Realtek Corporation. */
#ifndef RTW89_RADIO_TABLES_H
#define RTW89_RADIO_TABLES_H

/* Linux v6.17 rtw89 fw.h/fw.c and phy.c: firmware-appended resources are
 * separate from the executable MFW suits. NULL chip tables do NOT mean these
 * resources may be skipped. No function here enables transmission. The caller
 * must retain the original firmware allocation while these views are used. */
typedef struct {
    const u8 *header;
    const u8 *data;
    size_t size;
    u32 id;
} rtw89_radio_element_t;

static inline u32 rtw89_radio_le32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline bool rtw89_radio_elements_begin(const u8 *p, size_t size,
                                             size_t *cursor) {
    if (!p || !cursor || size < 16 || p[0] != 0xff || !p[1]) return false;
    size_t end = 16 + (size_t)p[1] * 16;
    if (end > size) return false;
    size_t table_end = end;
    for (unsigned i = 0; i < p[1]; ++i) {
        const u8 *suit = p + 16 + i * 16;
        size_t off = rtw89_radio_le32(suit + 4);
        size_t len = rtw89_radio_le32(suit + 8);
        if (!len || off < table_end || off > size || len > size - off)
            return false;
        if (off + len > end) end = off + len;
    }
    if (end > (size_t)-1 - 15) return false;
    *cursor = (end + 15) & ~(size_t)15;
    return *cursor <= size;
}

/* 1 = element, 0 = clean end, -1 = bad.
 * Never reads fields before the complete fixed 32-byte header is available. */
static inline int rtw89_radio_element_next(const u8 *p, size_t size,
                                           size_t *cursor,
                                           rtw89_radio_element_t *out) {
    if (!p || !cursor || !out || *cursor > size) return -1;
    size_t off = *cursor, remain = size - off;
    if (!remain) return 0;
    if (remain < 32) return -1;
    size_t len = rtw89_radio_le32(p + off + 4);
    if (len > remain - 32) return -1;
    out->header = p + off;
    out->data = p + off + 32;
    out->size = len;
    out->id = rtw89_radio_le32(p + off);
    size_t end = off + 32 + len;
    if (end > (size_t)-1 - 15) return -1;
    size_t aligned = (end + 15) & ~(size_t)15;
    /* The final element need not have trailing alignment bytes. */
    if (aligned > size) {
        if (end != size) return -1;
        *cursor = size;
    } else {
        *cursor = aligned;
    }
    return 1;
}

/* BB MCU resources select the closest supported cut, independent of file
 * order. Other singleton resources require an unambiguous identity. */
static inline bool rtw89_fw_element_select(const u8 *whole, size_t whole_size,
                                            u8 cv, u8 element_id,
                                            const u8 **data, size_t *size) {
    if (!data || !size) return false;
    *data = 0;
    *size = 0;
    size_t cursor;
    if (!rtw89_radio_elements_begin(whole, whole_size, &cursor)) return false;
    rtw89_radio_element_t elm;
    const u8 *best = 0;
    size_t best_size = 0;
    u8 best_cv = 0;
    int status;
    while ((status = rtw89_radio_element_next(whole, whole_size, &cursor, &elm)) > 0) {
        if (elm.id != element_id) continue;
        if (element_id <= 1) {
            u8 cut = elm.header[24];
            if (cut > cv || (best && cut < best_cv)) continue;
            if (best && cut == best_cv) return false;
            best_cv = cut;
        } else if (best) {
            return false;
        }
        best = elm.data;
        best_size = elm.size;
    }
    if (status < 0 || !best || !best_size) return false;
    *data = best;
    *size = best_size;
    return true;
}

/* Callback gets only selected address/data rows, in original order. A NULL
 * callback performs validation only. Call validation before any hardware I/O;
 * hardware failures cannot be rolled back by a table interpreter. */
typedef bool (*rtw89_radio_row_fn)(void *ctx, u32 addr, u32 data);

static inline bool rtw89_radio_table_walk(const u8 *table, size_t bytes,
                                          u8 rfe, u8 cv,
                                          rtw89_radio_row_fn row, void *ctx) {
    if (!table || !bytes || bytes % 8) return false;
    size_t count = bytes / 8, headlines = 0, chosen = 0;
    while (headlines < count &&
           (rtw89_radio_le32(table + headlines * 8) >> 28) == 15) ++headlines;
    if (headlines == count) return false;
    if (headlines) {
        bool found = false;
        u32 exact = (u32)rfe << 16 | cv;
        u32 wildcard = (u32)rfe << 16 | 0xff;
        for (unsigned pass = 0; pass < 4 && !found; ++pass) {
            u8 max_cv = 0;
            for (size_t i = 0; i < headlines; ++i) {
                u32 target = rtw89_radio_le32(table + i * 8) & 0x0fffffff;
                if (pass < 2) {
                    if (target == (pass ? wildcard : exact)) {
                        chosen = i; found = true; break;
                    }
                } else if (((target >> 16) & 0xff) == (pass == 2 ? rfe : 0xff) &&
                           (u8)target >= max_cv) {
                    max_cv = (u8)target; chosen = i; found = true;
                }
            }
        }
        if (!found) return false;
    }
    u32 selected = rtw89_radio_le32(table + chosen * 8) & 0x0fffffff;
    u32 target = 0;
    bool matched = true, found = false, in_branch = false, pending = false;
    for (size_t i = headlines; i < count; ++i) {
        u32 addr = rtw89_radio_le32(table + i * 8);
        u32 value = rtw89_radio_le32(table + i * 8 + 4);
        switch (addr >> 28) {
        case 8:
            if (in_branch) return false;
            in_branch = true;
            target = addr & 0x0fffffff; pending = true;
            break;
        case 9:
            if (!in_branch || pending) return false;
            target = addr & 0x0fffffff; pending = true;
            break;
        case 10:
            if (!in_branch || pending || !found) return false;
            matched = false;
            break;
        case 11:
            if (!in_branch || pending) return false;
            matched = true; found = false; in_branch = false;
            break;
        case 4:
            if (!in_branch || !pending) return false;
            pending = false;
            matched = !found && target == selected;
            if (matched) found = true;
            break;
        default:
            if (pending || (addr >> 28)) return false;
            if (matched && row && !row(ctx, addr, value)) return false;
            break;
        }
    }
    return !in_branch && !pending;
}

static inline bool rtw89_radio_table_apply(const u8 *table, size_t bytes,
                                           u8 rfe, u8 cv,
                                           rtw89_radio_row_fn row, void *ctx) {
    if (!row || !rtw89_radio_table_walk(table, bytes, rfe, cv, 0, 0)) return false;
    return rtw89_radio_table_walk(table, bytes, rfe, cv, row, ctx);
}

typedef struct {
    rtw89_radio_element_t bb, gain, rf[2], nctl;
    /* IDs 9..17: by-rate; normal/RU limits for 2/5/6GHz; shaping limits. */
    rtw89_radio_element_t power[9], tracking;
    const u8 *bbmcu;
    size_t bbmcu_size;
    u32 recognized;
} rtw89_radio_resources_t;

/* Resource admission only: being present/valid does NOT mean that power
 * limits, calibration, gain offsets or channel programming have been applied.
 * Linux fw.c chooses last exact-RFE TX power resource, else last default RFE0.
 * RF table slot is reg2.idx, whereas RF hardware path comes from the element
 * ID. The packaged RTL8922A resource intentionally swaps those two orders. */
static inline bool rtw89_radio_resources_load(const u8 *whole, size_t size,
                                              u8 cv, u8 rfe,
                                              rtw89_radio_resources_t *out) {
    if (!out) return false;
    *out = (rtw89_radio_resources_t){0};
    rtw89_radio_resources_t r = {0};
    size_t cursor;
    if (rfe == 0xff || !rtw89_radio_elements_begin(whole, size, &cursor)) return false;
    rtw89_radio_element_t e;
    int status;
    while ((status = rtw89_radio_element_next(whole, size, &cursor, &e)) > 0) {
        rtw89_radio_element_t *slot = 0;
        if (e.id == 2) slot = &r.bb;
        else if (e.id == 3) slot = &r.gain;
        else if (e.id == 4 || e.id == 5) {
            if (e.header[24] >= 2) return false;
            slot = &r.rf[e.header[24]];
        } else if (e.id == 8) slot = &r.nctl;
        if (slot) {
            if (slot->data || !rtw89_radio_table_walk(e.data, e.size, rfe, cv, 0, 0))
                return false;
            *slot = e;
            r.recognized |= 1u << e.id;
        } else if (e.id >= 9 && e.id <= 17) {
            slot = &r.power[e.id - 9];
            u8 candidate_rfe = e.header[26], entry_bytes = e.header[27];
            if (candidate_rfe != rfe && candidate_rfe != 0) continue;
            if (candidate_rfe == 0 && slot->data && slot->header[26] != 0) continue;
            u32 entries = rtw89_radio_le32(e.header + 28);
            if (!entries || !entry_bytes || entries > e.size / entry_bytes)
                return false;
            *slot = e;
            r.recognized |= 1u << e.id;
        } else if (e.id == 18) {
            /* Four 6GHz tables x4 subbands, four 5GHz x3, eight 2GHz x1;
             * each delta-swing row has 30 signed byte entries. */
            if (r.tracking.data || rtw89_radio_le32(e.header + 24) != 0xffff ||
                e.size < (4 * 4 + 4 * 3 + 8) * 30) return false;
            r.tracking = e;
            r.recognized |= 1u << e.id;
        }
    }
    if (status < 0 || !rtw89_fw_element_select(whole, size, cv, 0,
                                               &r.bbmcu, &r.bbmcu_size)) return false;
    r.recognized |= 1;
    const u32 required = 0x7ff35u; /* RTW89_BE_GEN_DEF_NEEDED_FW_ELEMENTS */
    if ((r.recognized & required) != required || !r.rf[0].data || !r.rf[1].data)
        return false;
    *out = r;
    return true;
}

typedef struct {
    bool (*bb_write)(void *ctx, u32 address, u32 value);
    void (*delay_us)(void *ctx, u32 us);
    bool (*rf_write)(void *ctx, u8 path, u32 address, u32 value);
    /* Linux rtw89_fw_h2c_rf_reg: PHY category, class CONFIG_RF_A/B,
     * function=page, payload little-endian packed address/data words. */
    bool (*rf_page)(void *ctx, u8 path, u8 page, const u8 *bytes, size_t len);
    void *ctx;
    u32 bb_window; /* Bytes addressable relative to the PHY register base. */
} rtw89_radio_io_t;

typedef struct {
    const rtw89_radio_io_t *io;
    u8 path;
    bool execute;
    u8 *rf_words;
    size_t rf_count, rf_capacity;
} rtw89_radio_apply_t;

static inline bool rtw89_radio_bb_row(void *opaque, u32 address, u32 data) {
    rtw89_radio_apply_t *a = (rtw89_radio_apply_t *)opaque;
    static const u32 delays[] = {1, 5, 50, 1000, 5000, 50000};
    if (address >= 0xf9 && address <= 0xfe) {
        if (a->execute) a->io->delay_us(a->io->ctx, delays[address - 0xf9]);
        return true;
    }
    if (data == 0xbabecafe) return true;
    if ((address & 3) || a->io->bb_window < 4 || address > a->io->bb_window - 4)
        return false;
    return !a->execute || a->io->bb_write(a->io->ctx, address, data);
}

static inline bool rtw89_radio_apply_bb(const rtw89_radio_element_t *table,
                                        u8 rfe, u8 cv, const rtw89_radio_io_t *io) {
    /* BB_GAIN rows describe gain fields, never BB register addresses. */
    if (!table || (table->id != 2 && table->id != 8) || !io ||
        !io->bb_write || !io->delay_us) return false;
    rtw89_radio_apply_t a = {.io = io};
    if (!rtw89_radio_table_walk(table->data, table->size, rfe, cv,
                                 rtw89_radio_bb_row, &a)) return false;
    a.execute = true;
    return rtw89_radio_table_walk(table->data, table->size, rfe, cv,
                                   rtw89_radio_bb_row, &a);
}

static inline bool rtw89_radio_rf_row(void *opaque, u32 address, u32 data) {
    rtw89_radio_apply_t *a = (rtw89_radio_apply_t *)opaque;
    /* Firmware reg2 tables use config_rf_reg_v1, not the older callback with
     * delay sentinels. Its >=0x100 entries are also copied to firmware. */
    if (data > 0xfffff || address > 0x1ffff) return false;
    if (address >= 0x100 && a->rf_count >= a->rf_capacity) return false;
    if (a->execute && !a->io->rf_write(a->io->ctx, a->path, address, data)) return false;
    if (address >= 0x100) {
        if (a->execute) {
            u32 packed = (address << 20) | data;
            u8 *p = a->rf_words + a->rf_count * 4;
            for (unsigned i = 0; i < 4; ++i) p[i] = (u8)(packed >> (i * 8));
        }
        ++a->rf_count;
    }
    return true;
}

static inline bool rtw89_radio_apply_rf(const rtw89_radio_element_t *table,
                                        u8 rfe, u8 cv, const rtw89_radio_io_t *io,
                                        u8 *workspace, size_t workspace_size) {
    if (!table || (table->id != 4 && table->id != 5) || !io ||
        !io->rf_write || !io->rf_page || !workspace || workspace_size < 6000)
        return false;
    rtw89_radio_apply_t a = {.io = io, .path = (u8)(table->id - 4),
                             .rf_words = workspace, .rf_capacity = 1500};
    if (!rtw89_radio_table_walk(table->data, table->size, rfe, cv,
                                 rtw89_radio_rf_row, &a)) return false;
    a.execute = true;
    a.rf_count = 0;
    if (!rtw89_radio_table_walk(table->data, table->size, rfe, cv,
                                 rtw89_radio_rf_row, &a)) return false;
    for (size_t i = 0; i < a.rf_count; i += 500) {
        size_t words = a.rf_count - i;
        if (words > 500) words = 500;
        if (!io->rf_page(io->ctx, a.path, (u8)(i / 500), workspace + i * 4, words * 4))
            return false;
    }
    return true;
}
#endif
