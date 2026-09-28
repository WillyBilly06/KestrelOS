/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* BE gain data format adapted from Linux v6.17 rtw89/phy_be.c/core.h,
 * Copyright(c) 2024-2025 Realtek Corporation. No hardware access here. */
#ifndef RTW89_RADIO_GAIN_H
#define RTW89_RADIO_GAIN_H
#include "rtw89_radio_tables.h"

typedef struct {
    signed char lna_gain[12][2][2][7];
    signed char tia_gain[12][2][2][2];
    signed char lna_op1db[12][2][2][7];
    signed char tia_lna_op1db[12][2][2][8];
    signed char rpl_20[12][2][1];
    signed char rpl_40[12][2][2];
    signed char rpl_80[12][2][4];
    signed char rpl_160[12][2][8];
    /* Seven groups: LNA 0..3/4..6, TIA, OP1dB LNA0..3/4..6,
     * OP1dB TIA-LNA0..3/4..7. Missing fields are not measured zero gain. */
    u8 present[12][2][2];
    u8 rpl_present[12][2]; /* 20/40/80/160-low/160-high */
    u32 loaded_rows;
    u32 ignored_rows;
} rtw89_radio_gain_t;

typedef struct {rtw89_radio_gain_t *gain; u8 rfe;} rtw89_radio_gain_decode_t;

static inline bool rtw89_radio_gain_row(void *context, u32 address, u32 data) {
    rtw89_radio_gain_decode_t *d = (rtw89_radio_gain_decode_t *)context;
    unsigned type = address & 255, path = address >> 8 & 15;
    unsigned bw = address >> 12 & 15, band = address >> 16 & 255;
    unsigned config = address >> 24;
    if (band >= 12 || path >= 2 || bw >= 2 || (address >= 0xf9 && address <= 0xfe))
        return false;
    /* Linux intentionally ignores bypass and non-eFEM extension rows. */
    if (config == 2 || (config == 4 && d->rfe < 50)) {
        if (d->gain) ++d->gain->ignored_rows;
        return true;
    }
    signed char *dest = 0;
    unsigned start = 0, length = 0, present_bit = 0;
    if (config == 0 || config == 3) {
        if (type > (config ? 3u : 2u)) return false;
        if (type < 2) {
            start = type ? 4 : 0;
            length = type ? 3 : 4;
            present_bit = type + (config ? 3 : 0);
            if (d->gain) dest = config ? d->gain->lna_op1db[band][bw][path] :
                                         d->gain->lna_gain[band][bw][path];
        } else {
            start = type == 3 ? 4 : 0;
            length = config ? 4 : 2;
            present_bit = config ? type + 3 : 2;
            if (d->gain) dest = config ? d->gain->tia_lna_op1db[band][bw][path] :
                                         d->gain->tia_gain[band][bw][path];
        }
        if (d->gain) d->gain->present[band][bw][path] |= (u8)(1u << present_bit);
    } else if (config == 1) {
        unsigned sub_bw = type >> 4, half = type & 15;
        if (sub_bw > 3 || (sub_bw < 3 && half) || (sub_bw == 3 && half > 1))
            return false;
        if (sub_bw == 0) {length = 1; if (d->gain) dest = d->gain->rpl_20[band][path];}
        if (sub_bw == 1) {length = 2; if (d->gain) dest = d->gain->rpl_40[band][path];}
        if (sub_bw == 2) {length = 4; if (d->gain) dest = d->gain->rpl_80[band][path];}
        if (sub_bw == 3) {
            length = 4; start = half * 4;
            if (d->gain) dest = d->gain->rpl_160[band][path];
        }
        if (d->gain) d->gain->rpl_present[band][path] |= (u8)(1u << (sub_bw + (sub_bw == 3 ? half : 0)));
    } else {
        return false; /* Unsupported eFEM/unknown layout never becomes zero calibration. */
    }
    if (dest) {
        for (unsigned i = 0; i < length; ++i) dest[start + i] = (signed char)(data >> (i * 8));
        ++d->gain->loaded_rows;
    }
    return true;
}

static inline bool rtw89_radio_gain_decode(const rtw89_radio_element_t *table,
                                           u8 rfe, u8 cv, rtw89_radio_gain_t *out) {
    if (!out) return false;
    *out = (rtw89_radio_gain_t){0};
    if (!table || table->id != 3 || rfe == 0xff) return false;
    rtw89_radio_gain_decode_t context = {.rfe = rfe};
    if (!rtw89_radio_table_walk(table->data, table->size, rfe, cv,
                                 rtw89_radio_gain_row, &context)) return false;
    context.gain = out;
    return rtw89_radio_table_walk(table->data, table->size, rfe, cv,
                                   rtw89_radio_gain_row, &context);
}

static inline bool rtw89_radio_gain_complete(const rtw89_radio_gain_t *gain,
                                             u8 band, u8 bw_type, u8 path) {
    return gain && band < 12 && bw_type < 2 && path < 2 &&
           gain->present[band][bw_type][path] == 0x7f;
}
#endif
