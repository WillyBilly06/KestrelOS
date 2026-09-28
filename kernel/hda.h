/* hda.h - Intel High Definition Audio.
 *
 * HD Audio is the one sound interface worth implementing from nothing: since
 * about 2004 essentially every desktop, laptop and server has one, the register
 * layout is a published specification rather than a vendor secret, and the
 * codecs behind it describe themselves.  A driver written to the specification
 * works on Intel, AMD, NVIDIA and VIA controllers alike, because the controller
 * is the standard part and only the codec differs - and the codec can be asked
 * what it is.
 */
#ifndef KESTREL_HDA_H
#define KESTREL_HDA_H

#include "kernel.h"

void hda_init(void);

/* What the sound hardware is, for the System app and the `audio` tool. */
typedef struct {
    bool present;
    bool output_ready;
    u16  pci_vendor, pci_device;
    u32  codec_vendor_id;       /* vendor in the high half, device in the low */
    u8   codec_address;
    u8   dac_node, pin_node;
    u32  sample_rate;
    u8   channels;
    u8   bits;

    /* Recording, asked of its own converter: a codec that plays at 192 kHz
     * frequently records at less, so these are not a copy of the above. */
    bool input_ready;
    u8   adc_node, in_pin_node;
    u32  input_rate;
    u8   input_channels, input_bits;
    char controller[48];
    char codec[48];
    char note[96];
} hda_info_t;

bool hda_get_info(hda_info_t *out);

/* The formats the output converter offers, and picking one at runtime. */
int  hda_formats(u32 *rates, int max_rates, u8 *depths, int max_depths,
                 int *depth_count);
bool hda_set_format(u32 rate, u8 bits);

/* Queue signed 16-bit interleaved samples at the configured rate.  Returns the
 * number of bytes accepted, which is less than asked for when the ring is
 * full. */
int  hda_write(const void *samples, int bytes);
int  hda_capture_selftest(void);
int  hda_read(void *samples, int bytes);
int  hda_recorded(void);
void hda_capture_stop(void);


/* How much room is left, in bytes. */
int  hda_space(void);

/* How much is written but not yet played, and where the hardware has read to.
 * The position is what distinguishes a stream that is running from one that is
 * merely configured. */
int  hda_queued(void);
u32  hda_position(void);

/* Stop playing and discard whatever is queued. */
void hda_flush(void);

#endif
