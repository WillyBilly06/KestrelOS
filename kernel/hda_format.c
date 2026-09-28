/* hda_format.c - see hda_format.h.  Pure: no hardware, no kernel calls. */
#if defined(HDA_FORMAT_HOST_TEST)
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#else
#include "kernel.h"
#endif
#include "hda_format.h"

u16 hda_encode_format(u32 rate, u8 bits, u8 channels) {
    u16 base44 = 0, mult = 0, div = 0;

    /* mult/div are the raw field values; the real factor is field+1, which is
     * why 48/6 is div=5 and 48 x2 is mult=1.  96 and 88.2 kHz are the same
     * multiplier against the two different bases. */
    switch (rate) {
    case   8000: mult = 0; div = 5; break;         /* 48 / 6 */
    case  11025: base44 = 1; mult = 0; div = 3; break;
    case  16000: mult = 0; div = 2; break;         /* 48 / 3 */
    case  22050: base44 = 1; mult = 0; div = 1; break;
    case  32000: mult = 1; div = 2; break;         /* 48 x 2 / 3 */
    case  44100: base44 = 1; mult = 0; div = 0; break;
    case  48000: mult = 0; div = 0; break;
    case  88200: base44 = 1; mult = 1; div = 0; break;
    case  96000: mult = 1; div = 0; break;
    case 176400: base44 = 1; mult = 3; div = 0; break;
    case 192000: mult = 3; div = 0; break;
    default:     mult = 0; div = 0; break;         /* 48 kHz */
    }

    u16 depth;
    switch (bits) {
    case 8:  depth = 0; break;
    case 16: depth = 1; break;
    case 20: depth = 2; break;
    case 24: depth = 3; break;
    case 32: depth = 4; break;
    default: depth = 1; break;
    }

    return (u16)((base44 << 14) | (mult << 11) | (div << 8) |
                 (depth << 4) | (channels ? channels - 1 : 1));
}
