/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Linux v6.17 rtw89 fw.c/fw.h/core.h resource layouts and bounds.
 * Copyright(c) 2019-2025 Realtek Corporation. This is data preparation only:
 * it neither chooses a regulatory domain nor grants permission to transmit. */
#ifndef RTW89_RADIO_POWER_H
#define RTW89_RADIO_POWER_H
#include "rtw89_radio_tables.h"

typedef struct {
    u8 id, band, bw, nss, ofdma, rate_section, shift, length;
    u8 ntx, beamforming, regulation, power_6ghz, channel_index, ru;
    signed char rates[4];
    int value; /* Signed limit; unsigned shaping index for IDs16/17. */
} rtw89_radio_power_entry_t;

/* Accept the older zero-extended record layouts used by Linux, and future
 * larger records only when their unknown extension is entirely zero. */
static inline bool rtw89_radio_power_entry(u8 id, const u8 *bytes, size_t size,
                                          rtw89_radio_power_entry_t *out) {
    static const u8 widths[9] = {11, 7, 7, 8, 5, 5, 6, 4, 3};
    if (!out) return false;
    *out = (rtw89_radio_power_entry_t){0};
    if (!bytes || !size || id < 9 || id > 17) return false;
    unsigned width = widths[id - 9];
    /* The legacy by-rate record omitted only the trailing BW/OFDMA fields.
     * Do not manufacture a missing power value from a truncated entry. */
    if (size < (id == 9 ? 9u : width)) return false;
    u8 b[11] = {0};
    for (size_t i = 0; i < size; ++i) {
        if (i < width) b[i] = bytes[i];
        else if (bytes[i]) return false;
    }
    rtw89_radio_power_entry_t e = {.id = id};
    if (id == 9) {
        static const u8 limits[] = {4, 8, 16, 4, 8};
        e.band = b[0]; e.nss = b[1]; e.rate_section = b[2];
        e.shift = b[3]; e.length = b[4]; e.bw = b[9]; e.ofdma = b[10];
        if (e.band >= 3 || e.bw >= 5 || e.rate_section >= 5 ||
            !e.length || e.length > 4 || (unsigned)e.shift + e.length > limits[e.rate_section])
            return false;
        if ((e.rate_section == 2 && (e.nss >= 4 || e.ofdma >= 2)) ||
            (e.rate_section == 3 && (e.nss >= 2 || e.ofdma >= 2))) return false;
        for (unsigned i = 0; i < e.length; ++i) e.rates[i] = (signed char)b[5 + i];
    } else if (id <= 12) {
        static const u8 bw_limits[] = {2, 4, 5}, ch_limits[] = {14, 53, 120};
        e.band = id - 10; e.bw = b[0]; e.ntx = b[1]; e.rate_section = b[2];
        e.beamforming = b[3]; e.regulation = b[4];
        if (id == 12) {e.power_6ghz = b[5]; e.channel_index = b[6]; e.value = (signed char)b[7];}
        else {e.channel_index = b[5]; e.value = (signed char)b[6];}
        if (e.bw >= bw_limits[e.band] || e.ntx >= 2 || e.rate_section >= 3 ||
            e.beamforming >= 2 || e.regulation >= 16 || e.power_6ghz >= 3 ||
            e.channel_index >= ch_limits[e.band]) return false;
    } else if (id <= 15) {
        static const u8 ch_limits[] = {14, 53, 120};
        e.band = id - 13; e.ru = b[0]; e.ntx = b[1]; e.regulation = b[2];
        if (id == 15) {e.power_6ghz = b[3]; e.channel_index = b[4]; e.value = (signed char)b[5];}
        else {e.channel_index = b[3]; e.value = (signed char)b[4];}
        if (e.ru >= 5 || e.ntx >= 2 || e.regulation >= 16 || e.power_6ghz >= 3 ||
            e.channel_index >= ch_limits[e.band]) return false;
    } else {
        e.band = b[0];
        if (id == 16) {e.rate_section = b[1]; e.regulation = b[2]; e.value = b[3];}
        else {e.regulation = b[1]; e.value = b[2];}
        if (e.band >= 3 || e.rate_section >= 2 || e.regulation >= 16) return false;
    }
    *out = e;
    return true;
}

typedef bool (*rtw89_radio_power_fn)(void *ctx, const rtw89_radio_power_entry_t *entry);

/* Validate every entry before invoking any consumer. Keep the original
 * firmware bytes alive; resource admission has already selected exact-RFE or
 * default-RFE tables. A missing entry must remain unavailable, not implicit0. */
static inline bool rtw89_radio_power_walk(const rtw89_radio_element_t *table,
                                          rtw89_radio_power_fn visit, void *ctx) {
    if (!table || !table->header || !table->data || table->id < 9 || table->id > 17)
        return false;
    unsigned width = table->header[27];
    u32 count = rtw89_radio_le32(table->header + 28);
    if (!width || !count || count > table->size / width) return false;
    rtw89_radio_power_entry_t entry;
    for (u32 i = 0; i < count; ++i)
        if (!rtw89_radio_power_entry((u8)table->id, table->data + (size_t)i * width,
                                      width, &entry)) return false;
    if (!visit) return true;
    for (u32 i = 0; i < count; ++i) {
        if (!rtw89_radio_power_entry((u8)table->id, table->data + (size_t)i * width,
                                      width, &entry) || !visit(ctx, &entry)) return false;
    }
    return true;
}
#endif
