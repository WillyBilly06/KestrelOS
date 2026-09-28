/* hda_format.h - encode a sample format the way HD Audio hardware takes it.
 *
 * A rate is not written as a number: the stream-format word carries a base of
 * 48 or 44.1 kHz, a multiplier and a divisor, a sample depth, and a channel
 * count, each in its own field (Intel HDA spec 3.7.1).  Getting a field wrong
 * is a silent failure - the codec plays the user's chosen rate as noise - so
 * this pure encoder lives on its own and is checked against the spec's own
 * numbers on the host (tools/hda_format_host_test.c).  See hda.c for the
 * driver that programs the word this returns into the codec and the stream
 * descriptor together.
 */
#ifndef KESTREL_HDA_FORMAT_H
#define KESTREL_HDA_FORMAT_H

#if defined(HDA_FORMAT_HOST_TEST)
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#else
#include "kernel.h"
#endif

/* The 16-bit HDA stream-format word for `rate` Hz, `bits` per sample, and
 * `channels` channels.  A rate that neither base can reach falls back to
 * 48 kHz; an unknown depth to 16-bit; zero channels to two. */
u16 hda_encode_format(u32 rate, u8 bits, u8 channels);

#endif /* KESTREL_HDA_FORMAT_H */
