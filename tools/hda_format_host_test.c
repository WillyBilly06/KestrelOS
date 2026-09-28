/* hda_format_host_test.c - check the HDA stream-format encoder off-target.
 *
 * "Allow the user to choose sample rate and bit rate" comes down to one 16-bit
 * word (Intel HDA spec 3.7.1): a wrong field there is silent - the codec plays
 * the chosen rate as noise.  This runs the UNMODIFIED hda_encode_format from
 * kernel/hda_format.c two ways: against the exact words the spec prescribes for
 * the formats the Settings app offers, and by DECODING each word's base /
 * multiplier / divisor back to a frequency and checking it equals the rate
 * asked for.  So a transcription slip in a field position or a base/mult/div
 * value fails here rather than on a speaker.
 *
 * Build + run (from the repo root):
 *   clang -std=c11 -Wall -Wextra -DHDA_FORMAT_HOST_TEST \
 *         -I kernel tools/hda_format_host_test.c kernel/hda_format.c \
 *         -o hda_format_test
 *   ./hda_format_test
 */
#ifndef HDA_FORMAT_HOST_TEST
#define HDA_FORMAT_HOST_TEST
#endif
#include <stdint.h>
#include <stdio.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#include "hda_format.h"

static int fails = 0, total = 0;
static void ck(const char *what, int cond) {
    total++;
    if (!cond) { fails++; printf("  FAIL: %s\n", what); }
    else        printf("  ok:   %s\n", what);
}

/* Decode a format word back to a frequency, per the spec's own fields, so the
 * check does not just re-implement the encoder's own table. */
static u32 decode_rate(u16 fmt) {
    u32 base = (fmt & (1u << 14)) ? 44100 : 48000;
    u32 mult = ((fmt >> 11) & 0x7) + 1;   /* field value + 1 */
    u32 div  = ((fmt >> 8)  & 0x7) + 1;
    /* 44.1-base rates are 44100*m/d; the /1000 keeps it integer for 44.1k. */
    if (base == 44100) return (u32)((44100ull * mult) / div);
    return base * mult / div;
}
static u32 decode_bits(u16 fmt) {
    switch ((fmt >> 4) & 0x7) {
    case 0: return 8; case 1: return 16; case 2: return 20;
    case 3: return 24; case 4: return 32; default: return 0;
    }
}
static u32 decode_channels(u16 fmt) { return (fmt & 0xF) + 1; }

int main(void) {
    /* The exact words the spec prescribes, computed by hand from the field
     * layout: base<<14 | mult<<11 | div<<8 | bits<<4 | (ch-1). */
    struct { u32 rate; u8 bits; u8 ch; u16 want; const char *note; } spec[] = {
        {  48000, 16, 2, 0x0011, "the format every codec must accept" },
        {  44100, 16, 2, 0x4011, "44.1k base"                          },
        {  96000, 24, 2, 0x0831, "48k x2, 24-bit"                      },
        {  88200, 16, 2, 0x4811, "44.1k x2"                            },
        { 192000, 32, 2, 0x1841, "48k x4, 32-bit"                      },
        { 176400, 24, 2, 0x5831, "44.1k x4, 24-bit"                    },
        {   8000, 16, 2, 0x0511, "48k / 6"                             },
        {  32000, 16, 2, 0x0A11, "48k x2 / 3"                          },
        {  48000, 16, 6, 0x0015, "5.1 channels"                        },
    };
    printf("[exact spec words]\n");
    for (size_t i = 0; i < sizeof spec / sizeof spec[0]; i++) {
        u16 got = hda_encode_format(spec[i].rate, spec[i].bits, spec[i].ch);
        char label[96];
        snprintf(label, sizeof label, "%u Hz / %u-bit / %uch -> 0x%04X (%s)",
                 spec[i].rate, spec[i].bits, spec[i].ch, spec[i].want, spec[i].note);
        ck(label, got == spec[i].want);
        if (got != spec[i].want)
            printf("        got 0x%04X, want 0x%04X\n", got, spec[i].want);
    }

    /* Every rate the Settings app can offer must survive a round trip: encode
     * it, then decode the word back to a frequency and a depth. */
    printf("[round trip - decode the word back]\n");
    static const u32 rates[] = { 8000, 11025, 16000, 22050, 32000, 44100,
                                 48000, 88200, 96000, 176400, 192000 };
    static const u8  depths[] = { 8, 16, 20, 24, 32 };
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        for (size_t b = 0; b < sizeof depths / sizeof depths[0]; b++) {
            u16 fmt = hda_encode_format(rates[r], depths[b], 2);
            char label[96];
            snprintf(label, sizeof label, "%u Hz round-trips", rates[r]);
            /* 44.1k-derived rates lose <0.05%% to integer division; allow 1 Hz. */
            u32 dr = decode_rate(fmt);
            int rate_ok = (dr > rates[r] ? dr - rates[r] : rates[r] - dr) <= 1;
            int bits_ok = decode_bits(fmt) == depths[b];
            int ch_ok   = decode_channels(fmt) == 2;
            if (b == 0) ck(label, rate_ok);   /* report the rate once per rate */
            if (!(rate_ok && bits_ok && ch_ok)) {
                fails++; total++;
                printf("  FAIL: %u Hz/%u-bit -> 0x%04X decodes %u Hz/%u-bit/%uch\n",
                       rates[r], depths[b], fmt, dr, decode_bits(fmt),
                       decode_channels(fmt));
            }
        }
    }

    /* Unknown inputs must fall back safely, not scribble stray bits. */
    printf("[fallbacks]\n");
    ck("an unreachable rate falls back to 48 kHz", decode_rate(hda_encode_format(384000, 16, 2)) == 48000);
    ck("an odd depth falls back to 16-bit",        decode_bits(hda_encode_format(48000, 17, 2)) == 16);

    printf("\n%d/%d checks passed%s\n", total - fails, total,
           fails ? "  <<< FAILURE" : "  ALL GOOD");
    return fails ? 1 : 0;
}
