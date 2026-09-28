/* hda.c - Intel High Definition Audio.
 *
 * The controller is the standard part: the same registers on Intel, AMD,
 * NVIDIA and VIA silicon, because the specification is published and everyone
 * implements it.  What differs is the codec, and the codec describes itself -
 * so the driver asks rather than being told.
 *
 * Bringing it up is four steps.  Reset the controller and let the codecs
 * announce themselves.  Set up CORB and RIRB, the two ring buffers commands and
 * responses travel through.  Walk the codec's widget graph to find a path from
 * a digital-to-analogue converter out to a pin that has something plugged into
 * it - which is the part that cannot be hard-coded, because every codec wires
 * its widgets differently.  Then hand the output stream a buffer descriptor
 * list and let it read samples out of memory on its own.
 *
 * Playback is a ring: the hardware walks a fixed buffer forever and reports
 * where it has reached, and the driver writes ahead of that position.  There is
 * no interrupt in the path - the position register is authoritative and cheap
 * to read, and a program that stops feeding it hears silence rather than a
 * stall.
 */
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "time.h"
#include "proc.h"
#include "vfs.h"
#include "klog.h"
#include "hda.h"
#include "hda_format.h"
#include "usb.h"

/* ------------------------------------------------------------- registers */

#define GCAP        0x00      /* capabilities                    */
#define GCTL        0x08      /* global control                  */
#define WAKEEN      0x0C
#define STATESTS    0x0E      /* which codecs responded to reset */
#define INTCTL      0x20
#define INTSTS      0x24

#define CORBLBASE   0x40
#define CORBUBASE   0x44
#define CORBWP      0x48
#define CORBRP      0x4A
#define CORBCTL     0x4C
#define CORBSIZE    0x4E

#define RIRBLBASE   0x50
#define RIRBUBASE   0x54
#define RIRBWP      0x58
#define RINTCNT     0x5A
#define RIRBCTL     0x5C
#define RIRBSTS     0x5D
#define RIRBSIZE    0x5E

#define DPLBASE     0x70
#define DPUBASE     0x74

#define GCTL_CRST   (1u << 0)     /* out of reset when set */

#define CORBCTL_RUN (1u << 1)
#define RIRBCTL_RUN (1u << 1)
#define RIRBCTL_RESPONSE_STATUS (1u << 0)

/* Stream descriptors start at 0x80, one per stream, 0x20 bytes apart.  Input
 * streams come first, then output streams; how many of each is in GCAP. */
#define SD_BASE     0x80
#define SD_STRIDE   0x20

#define SD_CTL      0x00      /* three bytes: run, reset, stream number */
#define SD_STS      0x03
#define SD_LPIB     0x04      /* how far the hardware has read           */
#define SD_CBL      0x08      /* cyclic buffer length                    */
#define SD_LVI      0x0C      /* last valid buffer descriptor index      */
#define SD_FIFOS    0x10
#define SD_FMT      0x12
#define SD_BDLPL    0x18
#define SD_BDLPU    0x1C

#define SDCTL_SRST  (1u << 0)
#define SDCTL_RUN   (1u << 1)
#define SDCTL_IOCE  (1u << 2)

/* --------------------------------------------------------- codec commands */

/* A verb is one 32-bit word: codec address, node, and either a 12-bit command
 * with an 8-bit payload or a 4-bit command with a 16-bit one. */
#define VERB(codec, node, verb, payload) \
    (((u32)(codec) << 28) | ((u32)(node) << 20) | \
     ((u32)(verb) << 8) | ((u32)(payload) & 0xFF))
#define VERB16(codec, node, verb, payload) \
    (((u32)(codec) << 28) | ((u32)(node) << 20) | \
     ((u32)(verb) << 16) | ((u32)(payload) & 0xFFFF))

#define VERB_GET_PARAM          0xF00
#define VERB_GET_CONNECT_SELECT 0xF01
#define VERB_SET_CONNECT_SELECT 0x701
#define VERB_GET_CONNECT_LIST   0xF02
#define VERB_GET_PIN_CTL        0xF07
#define VERB_SET_PIN_CTL        0x707
#define VERB_GET_EAPD           0xF0C
#define VERB_SET_EAPD           0x70C
#define VERB_GET_POWER_STATE    0xF05
#define VERB_SET_POWER_STATE    0x705
#define VERB_SET_CHANNEL_STREAM 0x706
#define VERB_GET_CONFIG_DEFAULT 0xF1C
#define VERB_SET_FORMAT         0x2      /* 16-bit payload */
#define VERB_SET_AMP_GAIN       0x3      /* 16-bit payload */
#define VERB_GET_AMP_GAIN       0xB

/* Parameters a node can be asked for. */
#define PARAM_VENDOR_ID     0x00
#define PARAM_REVISION      0x02
#define PARAM_NODE_COUNT    0x04
#define PARAM_FUNCTION_TYPE 0x05
#define PARAM_AUDIO_CAP     0x09
#define PARAM_PCM           0x0A     /* which rates and depths it will do */
#define PARAM_PIN_CAP       0x0C
#define PARAM_IN_AMP_CAP    0x0D
#define PARAM_CONNECT_LEN   0x0E
#define PARAM_OUT_AMP_CAP   0x12

#define FUNCTION_AUDIO      0x01

/* Widget types, from bits 23:20 of the audio widget capabilities. */
#define WIDGET_OUTPUT   0x0     /* a digital-to-analogue converter */
#define WIDGET_INPUT    0x1
#define WIDGET_MIXER    0x2
#define WIDGET_SELECTOR 0x3
#define WIDGET_PIN      0x4

#define PIN_CAP_OUTPUT  (1u << 4)
#define PIN_CAP_INPUT   (1u << 5)

/* A pin that records has to be told to listen, the same way one that plays has
 * to be told to drive. */
#define PIN_CTL_IN_ENABLE  (1u << 5)

/* Pin control bits. */
#define PIN_CTL_OUT_ENABLE (1u << 6)
#define PIN_CTL_HP_ENABLE  (1u << 7)

/* ------------------------------------------------------------------ state */

#define CORB_ENTRIES 256
#define RIRB_ENTRIES 256

/* One second of stereo 16-bit at 48 kHz, split into eight buffers so the
 * position register moves in reasonable steps. */
#define BDL_ENTRIES     8
#define BUFFER_BYTES    (48000 * 2 * 2)
#define SEGMENT_BYTES   (BUFFER_BYTES / BDL_ENTRIES)

typedef struct {
    u64 address;
    u32 length;
    u32 flags;
} __attribute__((packed)) bdl_entry_t;

static volatile u8 *regs;
static u32  *corb;   static u64 corb_phys;
static u64  *rirb;   static u64 rirb_phys;
static bdl_entry_t *bdl;  static u64 bdl_phys;
static u8   *audio_buf;   static u64 audio_phys;

static u16 corb_write_pos;
static u16 rirb_read_pos;

static u32 out_stream_base;      /* register offset of the output stream */
static u8  out_stream_number = 1;

static hda_info_t info;
static bool ready;

/* What the device turned out to be capable of, and is therefore being run at.
 * Decided when the codec is configured, from what the codec says - not chosen
 * here and imposed on it. */
static u32 stream_rate = 48000;
static u8  stream_bits = 16;
static u8  stream_channels = 2;
static u16 stream_format = 0x0011;

/* The recording side, which is a separate converter, a separate pin and a
 * separate stream descriptor - and separate capabilities.  A codec whose
 * output runs at 192 kHz frequently records at less, so the two are asked
 * independently rather than one being assumed from the other. */
static bool capture_ready;
static u32  capture_base;          /* its stream descriptor                 */
static u8   capture_stream_number = 2;
static u32  capture_rate = 48000;
static u8   capture_bits = 16;
static u8   capture_channels = 2;
static u16  capture_format = 0x0011;
static u8   adc_node, in_pin_node;
static u8  *capture_buffer;
static u64  capture_phys;
static bdl_entry_t *capture_bdl;
static u64  capture_bdl_phys;
static u8   in_streams_count;

/* Which codec was configured, and the output pin/converter currently in use -
 * kept so the user can switch the active output or input to another pin the
 * codec offers, not only take the one auto-chosen at start-up. */
static u8   sel_codec;
static u8   out_pin_node;
static u8   dac_node;
static bool out_muted;
static u8   afg_first, afg_total;   /* the audio group's widget range */

/* The output and input pins the user may switch between, gathered while the
 * codec is walked.  `device` is the HDA default-device type (0 line-out,
 * 1 speaker, 2 headphone, 8 line-in, 0xA mic...), which is what names each one
 * in the Sound settings; is_output separates the two lists. */
typedef struct { u8 node; u8 device; u8 is_output; } hda_endpoint_t;
static hda_endpoint_t endpoints[24];
static int endpoint_count;

/* Declared here because the codec is configured before these are defined, and
 * moving the definitions up would put the format decoding in the middle of the
 * codec walk it is used by. */
static void best_format(u8 codec, u8 node, u32 *rate_out, u8 *bits_out);

/* Ring offsets alone cannot say how much is outstanding: once the hardware
 * catches up with the writer, the difference between two offsets is
 * indistinguishable from a whole buffer's worth.  Running totals answer it
 * without ambiguity, and the wrap count is what turns the hardware's ring
 * offset back into one. */
static u32  write_pos;          /* offset into the ring                    */
static u64  written_total;      /* bytes ever handed to the ring           */
static u64  played_base;        /* whole buffers the hardware has completed */
static u32  last_position;      /* to notice the position wrapping         */
static bool playing;

/* --------------------------------------------------------------- mmio */

static inline u8  rd8(u32 o)  { return *(volatile u8  *)(regs + o); }
static inline u16 rd16(u32 o) { return *(volatile u16 *)(regs + o); }
static inline u32 rd32(u32 o) { return *(volatile u32 *)(regs + o); }
static inline void wr8(u32 o, u8 v)   { *(volatile u8  *)(regs + o) = v; }
static inline void wr16(u32 o, u16 v) { *(volatile u16 *)(regs + o) = v; }
static inline void wr32(u32 o, u32 v) { *(volatile u32 *)(regs + o) = v; }

/* ------------------------------------------------------- codec conversation */

/* Send one verb and wait for its response.  CORB and RIRB are rings the
 * hardware walks on its own, so a command is posted by advancing a write
 * pointer and the reply is picked up by watching the other one move. */
static bool codec_command(u32 verb, u32 *response) {
    u16 next = (u16)((corb_write_pos + 1) % CORB_ENTRIES);
    corb[next] = verb;
    __asm__ volatile("" ::: "memory");
    wr16(CORBWP, next);
    corb_write_pos = next;

    /* The response ring holds two words per entry: the payload and which codec
     * sent it. */
    for (int i = 0; i < 1000; i++) {
        u16 wp = (u16)(rd16(RIRBWP) & 0xFF);
        if (wp != rirb_read_pos) {
            rirb_read_pos = (u16)((rirb_read_pos + 1) % RIRB_ENTRIES);
            u64 entry = rirb[rirb_read_pos];
            if (response) *response = (u32)entry;
            wr8(RIRBSTS, 0x05);            /* acknowledge */
            return true;
        }
        timer_udelay(20);
    }
    return false;
}

static u32 codec_param(u8 codec, u8 node, u8 param) {
    u32 r = 0;
    if (!codec_command(VERB(codec, node, VERB_GET_PARAM, param), &r)) return 0;
    return r;
}

/* ------------------------------------------------------------ widget graph */

/* A pin's configuration default says what the machine's designer wired it to.
 * The port connectivity field distinguishes a jack on the case from a fixed
 * internal speaker from a pin that is not connected at all. */
static bool pin_is_usable_output(u8 codec, u8 node) {
    u32 caps = codec_param(codec, node, PARAM_PIN_CAP);
    if (!(caps & PIN_CAP_OUTPUT)) return false;

    u32 config = 0;
    codec_command(VERB(codec, node, VERB_GET_CONFIG_DEFAULT, 0), &config);

    u8 connectivity = (u8)((config >> 30) & 0x3);
    if (connectivity == 1) return false;          /* nothing wired to it */

    /* Device type: 0 line out, 1 speaker, 2 headphone.  Anything else is an
     * input or a digital port this driver does not drive. */
    u8 device = (u8)((config >> 20) & 0xF);
    return device <= 2;
}

/* Follow a pin back through the graph to a converter.  Mixers and selectors sit
 * in between on most codecs, and which of their inputs is live differs per
 * machine, so the path has to be walked rather than assumed. */
static u8 find_dac_behind(u8 codec, u8 node, int depth) {
    if (depth > 6) return 0;

    u32 caps = codec_param(codec, node, PARAM_AUDIO_CAP);
    u8 type = (u8)((caps >> 20) & 0xF);
    if (type == WIDGET_OUTPUT) return node;

    u32 len = codec_param(codec, node, PARAM_CONNECT_LEN);
    u8 count = (u8)(len & 0x7F);
    bool long_form = (len & 0x80) != 0;
    if (!count) return 0;

    for (u8 i = 0; i < count && i < 16; i++) {
        u32 list = 0;
        u8 index = long_form ? (u8)(i / 2) : (u8)(i / 4);
        if (!codec_command(VERB(codec, node, VERB_GET_CONNECT_LIST,
                                (u8)(index * (long_form ? 2 : 4))), &list))
            continue;

        u16 entry = long_form ? (u16)((list >> ((i % 2) * 16)) & 0xFFFF)
                              : (u16)((list >> ((i % 4) * 8)) & 0xFF);
        /* A range entry marks the end of a run rather than a node. */
        if (long_form ? (entry & 0x8000) : (entry & 0x80)) continue;
        if (!entry) continue;

        u8 found = find_dac_behind(codec, (u8)entry, depth + 1);
        if (found) {
            /* A selector has to be told which input to take. */
            if (type == WIDGET_SELECTOR)
                codec_command(VERB(codec, node, VERB_SET_CONNECT_SELECT, i), NULL);
            return found;
        }
    }
    return 0;
}

/* Unmute and turn up a widget's amplifier.  The payload sets which amplifier
 * and which channel; both output and input amps are set because a mixer in the
 * path mutes on its input side. */
static void set_gain(u8 codec, u8 node, bool output, u8 gain) {
    u16 payload = (u16)((output ? 0x8000 : 0x4000) | 0x3000 | gain);
    codec_command(VERB16(codec, node, VERB_SET_AMP_GAIN, payload), NULL);
}

static u8 max_gain(u8 codec, u8 node, bool output) {
    u32 caps = codec_param(codec, node, output ? PARAM_OUT_AMP_CAP : PARAM_IN_AMP_CAP);
    u8 steps = (u8)((caps >> 8) & 0x7F);
    return steps ? steps : 0x4A;
}

/* Find a working output path on this codec. */
/* A pin that can record, and is either something plugged in or built in.
 *
 * The same reasoning as the output side, in reverse: a jack with nothing in it
 * is still a usable input and will simply be silent, so it is not rejected -
 * but a built-in microphone is preferred, because on a machine with nothing
 * plugged in that is the one that hears anything. */
static bool pin_is_usable_input(u8 codec, u8 node) {
    u32 caps = codec_param(codec, node, PARAM_PIN_CAP);
    return (caps & PIN_CAP_INPUT) != 0;
}

/* Walk back from a pin to the converter that records it, the mirror of
 * find_dac_behind.  A recording path runs the other way - the pin is the
 * source and the converter is the destination - so it is followed by asking
 * each input converter what it is connected to rather than by following the
 * pin's own connection list. */
static u8 find_adc_for(u8 codec, u8 pin, u8 first, u8 total) {
    for (u8 n = first; n < first + total; n++) {
        u32 caps = codec_param(codec, n, PARAM_AUDIO_CAP);
        if (((caps >> 20) & 0xF) != WIDGET_INPUT) continue;

        u32 len = codec_param(codec, n, PARAM_CONNECT_LEN);
        u8 count = (u8)(len & 0x7F);
        bool long_form = (len & 0x80) != 0;
        if (!count) continue;
        if (count > 32) count = 32;

        for (u8 i = 0; i < count; i++) {
            u32 entry = 0;
            codec_command(VERB(codec, n, VERB_GET_CONNECT_LIST,
                               long_form ? (u8)(i & ~1u) : (u8)(i & ~3u)), &entry);

            u8 item;
            if (long_form) item = (u8)((entry >> ((i & 1) * 16)) & 0xFF);
            else           item = (u8)((entry >> ((i & 3) * 8)) & 0xFF);

            if (item == pin) {
                /* A converter with several sources has to be told which one. */
                if (count > 1)
                    codec_command(VERB(codec, n, VERB_SET_CONNECT_SELECT, i), NULL);
                return n;
            }
        }
    }
    return 0;
}

/* Find and prepare a recording path, if the codec has one.  Failing to find
 * one is not a failure of the codec: plenty of machines have no input at all,
 * and saying so is different from saying the driver did not work. */
/* Route recording from a given input pin: find the converter behind it, tell
 * the pin to listen and open the path, and run the converter at what it can do.
 * The old input pin is told to stop listening so a switch moves the source.
 * Called with the auto-chosen pin at start-up and with the user's pick. */
static bool activate_input(u8 codec, u8 pin, u8 first, u8 total) {
    u8 adc = find_adc_for(codec, pin, first, total);
    if (!adc) return false;

    codec_command(VERB(codec, adc, VERB_SET_POWER_STATE, 0), NULL);
    codec_command(VERB(codec, pin, VERB_SET_POWER_STATE, 0), NULL);
    timer_mdelay(10);

    /* Stop the previous input pin listening (unless it is this one). */
    if (in_pin_node && in_pin_node != pin)
        codec_command(VERB(codec, in_pin_node, VERB_SET_PIN_CTL, 0), NULL);

    /* Tell the pin to listen, and open both gains on the path. */
    codec_command(VERB(codec, pin, VERB_SET_PIN_CTL, PIN_CTL_IN_ENABLE), NULL);
    set_gain(codec, adc, true, max_gain(codec, adc, true));
    set_gain(codec, pin, false, max_gain(codec, pin, false));

    /* And run it at the best the recording converter says it can do, asked of
     * that converter rather than assumed from the playback one. */
    best_format(codec, adc, &capture_rate, &capture_bits);
    capture_format = hda_encode_format(capture_rate, capture_bits, capture_channels);
    codec_command(VERB16(codec, adc, VERB_SET_FORMAT, capture_format), NULL);
    codec_command(VERB(codec, adc, VERB_SET_CHANNEL_STREAM,
                       (u32)(capture_stream_number << 4)), NULL);

    adc_node = adc;
    in_pin_node = pin;
    return true;
}

static void configure_input(u8 codec, u8 first, u8 total) {
    u8 best_pin = 0;
    int best_score = -1;

    for (u8 n = first; n < first + total; n++) {
        u32 caps = codec_param(codec, n, PARAM_AUDIO_CAP);
        if (((caps >> 20) & 0xF) != WIDGET_PIN) continue;
        if (!pin_is_usable_input(codec, n)) continue;

        u32 config = 0;
        codec_command(VERB(codec, n, VERB_GET_CONFIG_DEFAULT, 0), &config);
        u8 device = (u8)((config >> 20) & 0xF);
        u8 connectivity = (u8)((config >> 30) & 0x3);

        /* Device 0xA is a microphone and 0x8 is a line in. */
        int score = (device == 0xA) ? 3 : (device == 0x8) ? 2 : 1;
        if (connectivity == 2) score += 1;            /* built in */
        if (score > best_score) { best_score = score; best_pin = n; }
    }
    if (!best_pin) return;

    activate_input(codec, best_pin, first, total);
}

/* Route playback to a given output pin: open its whole path (power, gains, pin
 * output enable, external amp), bind the converter behind it to the output
 * stream and set the format, and silence the pin we were using before so a
 * switch moves the sound rather than doubling it.  derive_format re-asks the
 * converter what it will do (used at start-up); a user switch keeps the format
 * already chosen so their sample rate and bit depth survive the change. */
static bool activate_output(u8 codec, u8 pin, bool derive_format) {
    u8 dac = find_dac_behind(codec, pin, 0);
    if (!dac) return false;

    codec_command(VERB(codec, dac, VERB_SET_POWER_STATE, 0), NULL);
    codec_command(VERB(codec, pin, VERB_SET_POWER_STATE, 0), NULL);
    timer_mdelay(10);

    set_gain(codec, dac, true, max_gain(codec, dac, true));
    set_gain(codec, pin, true, max_gain(codec, pin, true));
    set_gain(codec, pin, false, max_gain(codec, pin, false));

    u32 ctl = 0;
    codec_command(VERB(codec, pin, VERB_GET_PIN_CTL, 0), &ctl);
    codec_command(VERB(codec, pin, VERB_SET_PIN_CTL,
                       (u8)(ctl | PIN_CTL_OUT_ENABLE | PIN_CTL_HP_ENABLE)), NULL);
    codec_command(VERB(codec, pin, VERB_SET_EAPD, 0x02), NULL);

    /* Silence the previous output pin (unless it is this one). */
    if (out_pin_node && out_pin_node != pin) {
        u32 pc = 0;
        codec_command(VERB(codec, out_pin_node, VERB_GET_PIN_CTL, 0), &pc);
        codec_command(VERB(codec, out_pin_node, VERB_SET_PIN_CTL,
                           (u8)(pc & ~(PIN_CTL_OUT_ENABLE | PIN_CTL_HP_ENABLE))), NULL);
    }

    codec_command(VERB(codec, dac, VERB_SET_CHANNEL_STREAM,
                       (u8)((out_stream_number << 4) | 0)), NULL);
    if (derive_format) {
        best_format(codec, dac, &stream_rate, &stream_bits);
        stream_format = hda_encode_format(stream_rate, stream_bits, stream_channels);
    }
    codec_command(VERB16(codec, dac, VERB_SET_FORMAT, stream_format), NULL);

    out_pin_node = pin;
    dac_node = dac;
    info.dac_node = dac;
    info.pin_node = pin;
    return true;
}

static bool configure_codec(u8 codec) {
    u32 id = codec_param(codec, 0, PARAM_VENDOR_ID);
    if (!id || id == 0xFFFFFFFF) return false;

    /* The root node lists the function groups; one of them is the audio one. */
    u32 count = codec_param(codec, 0, PARAM_NODE_COUNT);
    u8 first = (u8)((count >> 16) & 0xFF);
    u8 total = (u8)(count & 0xFF);

    u8 afg = 0;
    for (u8 n = first; n < first + total; n++) {
        u32 type = codec_param(codec, n, PARAM_FUNCTION_TYPE);
        if ((type & 0x7F) == FUNCTION_AUDIO) { afg = n; break; }
    }
    if (!afg) return false;

    /* Power the group up before anything under it is asked about. */
    codec_command(VERB(codec, afg, VERB_SET_POWER_STATE, 0), NULL);
    timer_mdelay(10);

    count = codec_param(codec, afg, PARAM_NODE_COUNT);
    first = (u8)((count >> 16) & 0xFF);
    total = (u8)(count & 0xFF);
    if (!total || total > 64) return false;

    /* Prefer a speaker or line out over a headphone jack: on a machine with
     * nothing plugged in, the speaker is the one that makes a sound. */
    u8 best_pin = 0;
    int best_score = -1;

    for (u8 n = first; n < first + total; n++) {
        u32 caps = codec_param(codec, n, PARAM_AUDIO_CAP);
        if (((caps >> 20) & 0xF) != WIDGET_PIN) continue;
        if (!pin_is_usable_output(codec, n)) continue;

        u32 config = 0;
        codec_command(VERB(codec, n, VERB_GET_CONFIG_DEFAULT, 0), &config);
        u8 device = (u8)((config >> 20) & 0xF);
        u8 connectivity = (u8)((config >> 30) & 0x3);

        int score = (device == 1) ? 3 : (device == 0) ? 2 : 1;
        if (connectivity == 2) score += 1;        /* fixed internal device */
        if (score > best_score) { best_score = score; best_pin = n; }
    }
    if (!best_pin) return false;

    /* Gather every output and input pin the codec offers, so the user can pick
     * a different one later (Sound settings).  Done here, while the graph is
     * already being walked, rather than re-walked on demand. */
    endpoint_count = 0;
    out_pin_node = 0;
    in_pin_node = 0;
    sel_codec = codec;
    afg_first = first;
    afg_total = total;
    for (u8 n = first; n < first + total && endpoint_count < 24; n++) {
        u32 caps = codec_param(codec, n, PARAM_AUDIO_CAP);
        if (((caps >> 20) & 0xF) != WIDGET_PIN) continue;
        u32 config = 0;
        codec_command(VERB(codec, n, VERB_GET_CONFIG_DEFAULT, 0), &config);
        u8 device = (u8)((config >> 20) & 0xF);
        if (pin_is_usable_output(codec, n))
            endpoints[endpoint_count++] = (hda_endpoint_t){ n, device, 1 };
        else if (pin_is_usable_input(codec, n))
            endpoints[endpoint_count++] = (hda_endpoint_t){ n, device, 0 };
    }

    /* Open the auto-chosen output path (power, gains, pin enable, converter). */
    if (!activate_output(codec, best_pin, true)) return false;

    /* And a recording path, if this codec has one.  Looked for after the
     * playback path because a codec with no input is common and must not stop
     * the output being set up. */
    configure_input(codec, first, total);

    info.codec_address = codec;
    info.codec_vendor_id = id;
    return true;
}

/* Codec makers, so the System app can name what it found. */
static const char *codec_vendor_name(u16 vendor) {
    switch (vendor) {
    case 0x10EC: return "Realtek";
    case 0x8384: return "SigmaTel";
    case 0x11D4: return "Analog Devices";
    case 0x14F1: return "Conexant";
    case 0x1102: return "Creative";
    case 0x1013: return "Cirrus Logic";
    case 0x10DE: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x1002: return "AMD";
    case 0x15AD: return "VMware";
    default:     return NULL;
    }
}

/* ---------------------------------------------------------------- bring-up */

static bool reset_controller(void) {
    /* Taking CRST low and back up resets the link and makes every codec
     * announce itself in STATESTS. */
    wr32(GCTL, rd32(GCTL) & ~GCTL_CRST);
    for (int i = 0; i < 100; i++) {
        if (!(rd32(GCTL) & GCTL_CRST)) break;
        timer_mdelay(1);
    }
    timer_mdelay(1);

    wr32(GCTL, rd32(GCTL) | GCTL_CRST);
    for (int i = 0; i < 100; i++) {
        if (rd32(GCTL) & GCTL_CRST) break;
        timer_mdelay(1);
    }
    if (!(rd32(GCTL) & GCTL_CRST)) {
        kerr("hda", "the controller will not come out of reset");
        return false;
    }

    /* Codecs need a moment to appear after the link comes up. */
    timer_mdelay(25);
    return true;
}

static bool setup_rings(void) {
    u64 phys;
    corb = dma_alloc_pages(1, &phys);
    if (!corb) return false;
    corb_phys = phys;

    rirb = dma_alloc_pages(1, &phys);
    if (!rirb) return false;
    rirb_phys = phys;

    /* Stop both rings before moving them. */
    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);

    wr32(CORBLBASE, (u32)corb_phys);
    wr32(CORBUBASE, (u32)(corb_phys >> 32));
    wr8(CORBSIZE, 0x02);                    /* 256 entries */

    /* Resetting the read pointer needs the reset bit set, then cleared. */
    wr16(CORBRP, 0x8000);
    for (int i = 0; i < 100 && !(rd16(CORBRP) & 0x8000); i++) timer_mdelay(1);
    wr16(CORBRP, 0);
    for (int i = 0; i < 100 && (rd16(CORBRP) & 0x8000); i++) timer_mdelay(1);
    wr16(CORBWP, 0);
    corb_write_pos = 0;

    wr32(RIRBLBASE, (u32)rirb_phys);
    wr32(RIRBUBASE, (u32)(rirb_phys >> 32));
    wr8(RIRBSIZE, 0x02);
    wr16(RIRBWP, 0x8000);                   /* reset the write pointer */
    wr16(RINTCNT, 1);
    rirb_read_pos = 0;

    /* Poll responses, but enable their status latch so acknowledging RIRBSTS
     * also releases the response-count throttle. With RUN alone QEMU stops
     * CORB after the first response at RINTCNT=1. Global IRQ delivery stays off. */
    wr32(INTCTL, 0);
    wr8(RIRBSTS, 0x05);
    wr8(RIRBCTL, RIRBCTL_RUN | RIRBCTL_RESPONSE_STATUS);
    wr8(CORBCTL, CORBCTL_RUN);
    return true;
}

static bool setup_stream(void) {
    u64 phys;
    bdl = dma_alloc_pages(1, &phys);
    if (!bdl) return false;
    bdl_phys = phys;

    size_t pages = (BUFFER_BYTES + PAGE_SIZE - 1) / PAGE_SIZE;
    audio_buf = dma_alloc_pages(pages, &phys);
    if (!audio_buf) return false;
    audio_phys = phys;
    memset(audio_buf, 0, BUFFER_BYTES);

    for (int i = 0; i < BDL_ENTRIES; i++) {
        bdl[i].address = audio_phys + (u64)i * SEGMENT_BYTES;
        bdl[i].length = SEGMENT_BYTES;
        bdl[i].flags = 0;                   /* no interrupt: the position
                                             * register is what is read */
    }

    /* Reset the stream before programming it. */
    wr8(out_stream_base + SD_CTL, SDCTL_SRST);
    for (int i = 0; i < 100; i++) {
        if (rd8(out_stream_base + SD_CTL) & SDCTL_SRST) break;
        timer_mdelay(1);
    }
    wr8(out_stream_base + SD_CTL, 0);
    for (int i = 0; i < 100; i++) {
        if (!(rd8(out_stream_base + SD_CTL) & SDCTL_SRST)) break;
        timer_mdelay(1);
    }

    wr32(out_stream_base + SD_CBL, BUFFER_BYTES);
    wr16(out_stream_base + SD_LVI, BDL_ENTRIES - 1);
    wr32(out_stream_base + SD_BDLPL, (u32)bdl_phys);
    wr32(out_stream_base + SD_BDLPU, (u32)(bdl_phys >> 32));

    /* The same format the codec was given, which is the best the codec said
     * it could do rather than a figure chosen here. */
    wr16(out_stream_base + SD_FMT, stream_format);

    /* The stream number in the top nibble of the third control byte is what
     * ties this stream to the converter that was bound to it. */
    wr8(out_stream_base + SD_CTL + 2, (u8)(out_stream_number << 4));
    return true;
}

/* The recording stream's own buffer and descriptor list.
 *
 * Recording is not playback with the arrows reversed as far as the controller
 * is concerned: it is a separate stream descriptor, with its own buffer that
 * the hardware writes and software reads.  Input stream descriptors come first
 * in the register block, which is why the output ones had to be found by
 * counting past them.
 */
static bool setup_capture_stream(void) {
    if (!in_streams_count) return false;

    capture_base = SD_BASE;                      /* the first input stream */

    u64 phys;
    capture_bdl = dma_alloc_pages(1, &phys);
    if (!capture_bdl) return false;
    capture_bdl_phys = phys;

    size_t pages = (BUFFER_BYTES + PAGE_SIZE - 1) / PAGE_SIZE;
    capture_buffer = dma_alloc_pages(pages, &phys);
    if (!capture_buffer) return false;
    capture_phys = phys;
    memset(capture_buffer, 0, BUFFER_BYTES);

    for (int i = 0; i < BDL_ENTRIES; i++) {
        capture_bdl[i].address = capture_phys + (u64)i * SEGMENT_BYTES;
        capture_bdl[i].length = SEGMENT_BYTES;
        capture_bdl[i].flags = 0;
    }

    wr8(capture_base + SD_CTL, SDCTL_SRST);
    for (int i = 0; i < 100; i++) {
        if (rd8(capture_base + SD_CTL) & SDCTL_SRST) break;
        timer_mdelay(1);
    }
    wr8(capture_base + SD_CTL, 0);
    for (int i = 0; i < 100; i++) {
        if (!(rd8(capture_base + SD_CTL) & SDCTL_SRST)) break;
        timer_mdelay(1);
    }

    wr32(capture_base + SD_CBL, BUFFER_BYTES);
    wr16(capture_base + SD_LVI, BDL_ENTRIES - 1);
    wr32(capture_base + SD_BDLPL, (u32)capture_bdl_phys);
    wr32(capture_base + SD_BDLPU, (u32)(capture_bdl_phys >> 32));
    wr16(capture_base + SD_FMT, capture_format);
    wr8(capture_base + SD_CTL + 2, (u8)(capture_stream_number << 4));
    return true;
}

/* ------------------------------------------------------------- what it will do
 *
 * A converter says which sample rates and sample depths it supports as two
 * bitmaps in one parameter.  This driver used to ignore that and run
 * everything at 48 kHz and sixteen bits, which is the one format every codec
 * supports - and therefore the one that throws away whatever the device in
 * front of it was actually capable of.  A codec that will do 192 kHz at 24
 * bits was being driven at a quarter of its rate and two thirds of its depth,
 * and nothing anywhere said so.
 *
 * So the capability is read and the best the device offers is what gets used.
 */

/* The rates the specification defines, in the order of their bits, with the
 * value each one means.  Rates are not a range: a codec supports a specific
 * set, and the highest supported one is not always the highest defined one. */
static const struct { u8 bit; u32 hz; } pcm_rates[] = {
    {  0,   8000 }, {  1,  11025 }, {  2,  16000 }, {  3,  22050 },
    {  4,  32000 }, {  5,  44100 }, {  6,  48000 }, {  7,  88200 },
    {  8,  96000 }, {  9, 176400 }, { 10, 192000 }, { 11, 384000 },
};

/* Likewise the depths, in bits 16 and up of the same parameter. */
static const struct { u8 bit; u8 bits; } pcm_depths[] = {
    { 16, 8 }, { 17, 16 }, { 18, 20 }, { 19, 24 }, { 20, 32 },
};

/* The highest rate and depth a converter will accept.
 *
 * Falls back to 48 kHz and sixteen bits when the codec answers with nothing,
 * because a codec that will not say what it supports still has to be driven,
 * and that pair is the one the specification requires everything to accept.
 */
static void best_format(u8 codec, u8 node, u32 *rate_out, u8 *bits_out) {
    u32 caps = codec_param(codec, node, PARAM_PCM);

    u32 best_rate = 0;
    u8  best_bits = 0;

    for (size_t i = 0; i < sizeof pcm_rates / sizeof pcm_rates[0]; i++)
        if (caps & (1u << pcm_rates[i].bit)) best_rate = pcm_rates[i].hz;

    for (size_t i = 0; i < sizeof pcm_depths / sizeof pcm_depths[0]; i++)
        if (caps & (1u << pcm_depths[i].bit)) best_bits = pcm_depths[i].bits;

    if (!best_rate || !best_bits) {
        kwarn("hda", "node %u did not say what it supports (%08x); using the "
                     "format every codec must accept", node, caps);
        best_rate = 48000;
        best_bits = 16;
    }

    *rate_out = best_rate;
    *bits_out = best_bits;
}

/* The same pair, in the form the hardware takes, is encoded by
 * hda_encode_format in hda_format.c - pure, and checked against the HDA spec's
 * own field values on the host (tools/hda_format_host_test.c). */

/* ------------------------------------------------------------------ output */

/* Where the hardware has read up to, and how much it has played in total.
 * The position register only counts within one pass of the ring, so each time
 * it goes backwards another whole buffer has been consumed. */
static u32 hardware_position(void) {
    if (!ready) return 0;
    u32 p = rd32(out_stream_base + SD_LPIB);
    if (p >= BUFFER_BYTES) p = 0;

    if (playing && p < last_position) played_base += BUFFER_BYTES;
    last_position = p;
    return p;
}

static u64 played_so_far(void) {
    u32 p = hardware_position();
    u64 total = played_base + p;
    /* The hardware never plays more than was written: past that point it is
     * reading the silence left behind the writer. */
    return total > written_total ? written_total : total;
}

/* How many bytes are written but not yet played. */
int hda_queued(void) {
    if (!ready) return 0;
    u64 played = played_so_far();
    u64 outstanding = written_total - played;

    /* Nothing left to play: stop the stream rather than let it walk the ring
     * forever reading silence. */
    if (outstanding == 0 && playing) {
        wr8(out_stream_base + SD_CTL,
            (u8)(rd8(out_stream_base + SD_CTL) & ~SDCTL_RUN));
        playing = false;
    }
    return (int)outstanding;
}

/* Where the hardware has read to, which is the only proof that the stream is
 * actually running rather than merely configured. */
u32 hda_position(void) { return hardware_position(); }

int hda_space(void) {
    if (!ready) return 0;
    u64 outstanding = written_total - played_so_far();
    /* One segment is kept between the writer and the hardware so a late write
     * is never overtaken part-way through a buffer the hardware is reading. */
    int free_bytes = (int)(BUFFER_BYTES - outstanding) - SEGMENT_BYTES;
    return free_bytes > 0 ? free_bytes : 0;
}

int hda_write(const void *samples, int bytes) {
    if (!ready || bytes <= 0) return 0;

    int space = hda_space();
    if (bytes > space) bytes = space;
    if (bytes <= 0) return 0;

    const u8 *src = samples;
    int first = (int)(BUFFER_BYTES - write_pos);
    if (first > bytes) first = bytes;

    memcpy(audio_buf + write_pos, src, (size_t)first);
    if (bytes > first) memcpy(audio_buf, src + first, (size_t)(bytes - first));
    write_pos = (u32)((write_pos + bytes) % BUFFER_BYTES);
    written_total += (u64)bytes;

    /* Leave silence immediately after what was just written: if the writer
     * falls behind, the hardware reads that rather than replaying whatever was
     * in the ring a second ago. */
    u32 tail = write_pos;
    int quiet = SEGMENT_BYTES;
    while (quiet > 0) {
        int run = (int)(BUFFER_BYTES - tail);
        if (run > quiet) run = quiet;
        memset(audio_buf + tail, 0, (size_t)run);
        tail = (u32)((tail + run) % BUFFER_BYTES);
        quiet -= run;
    }

    if (!playing) {
        last_position = rd32(out_stream_base + SD_LPIB);
        if (last_position >= BUFFER_BYTES) last_position = 0;
        wr8(out_stream_base + SD_CTL, (u8)(rd8(out_stream_base + SD_CTL) | SDCTL_RUN));
        playing = true;
    }
    return bytes;
}

void hda_flush(void) {
    if (!ready) return;
    wr8(out_stream_base + SD_CTL, (u8)(rd8(out_stream_base + SD_CTL) & ~SDCTL_RUN));
    playing = false;
    memset(audio_buf, 0, BUFFER_BYTES);
    write_pos = 0;
    written_total = 0;
    played_base = 0;
    last_position = 0;
}

bool hda_get_info(hda_info_t *out) {
    if (!out) return false;
    *out = info;
    return info.present;
}

/* ------------------------------------------------------- choosing a format
 *
 * The driver has always asked the converter what it supports and driven it at
 * the best of that; these let the person at the desk see the same list and
 * pick from it.  Nothing here invents a rate: everything offered comes off
 * the converter's own PARAM_PCM word, the same word best_format reads.
 */

/* Every rate and depth the output converter will accept.  Returns how many
 * rates were written; depths land in a small parallel array.  A codec that
 * answers nothing gets the pair the specification requires of everyone. */
int hda_formats(u32 *rates, int max_rates, u8 *depths, int max_depths,
                int *depth_count) {
    if (!ready) return 0;

    u32 caps = codec_param(info.codec_address, info.dac_node, PARAM_PCM);
    int nr = 0, nd = 0;

    for (size_t i = 0; i < sizeof pcm_rates / sizeof pcm_rates[0]; i++)
        if ((caps & (1u << pcm_rates[i].bit)) && nr < max_rates)
            rates[nr++] = pcm_rates[i].hz;
    for (size_t i = 0; i < sizeof pcm_depths / sizeof pcm_depths[0]; i++)
        if ((caps & (1u << pcm_depths[i].bit)) && nd < max_depths)
            depths[nd++] = pcm_depths[i].bits;

    if (!nr && max_rates)  rates[nr++] = 48000;
    if (!nd && max_depths) depths[nd++] = 16;
    if (depth_count) *depth_count = nd;
    return nr;
}

/* Drive the output at a different rate or depth, immediately.
 *
 * The order is the one the specification lays out for a format change: stop
 * and reset the stream, tell the converter the new format, tell the stream
 * descriptor the same thing, and start again.  Whatever was queued is
 * discarded - a buffer of 48 kHz samples played at 96 kHz is not somebody's
 * music faster, it is noise. */
bool hda_set_format(u32 rate, u8 bits) {
    if (!ready) return false;

    /* Only what the converter itself has said it accepts. */
    u32 caps = codec_param(info.codec_address, info.dac_node, PARAM_PCM);
    bool rate_ok = false, bits_ok = false;
    for (size_t i = 0; i < sizeof pcm_rates / sizeof pcm_rates[0]; i++)
        if (pcm_rates[i].hz == rate && (caps & (1u << pcm_rates[i].bit)))
            rate_ok = true;
    for (size_t i = 0; i < sizeof pcm_depths / sizeof pcm_depths[0]; i++)
        if (pcm_depths[i].bits == bits && (caps & (1u << pcm_depths[i].bit)))
            bits_ok = true;
    if (!caps) { rate_ok = (rate == 48000); bits_ok = (bits == 16); }
    if (!rate_ok || !bits_ok) {
        kwarn("hda", "asked for %u Hz at %u bits, which this converter does "
                     "not offer", rate, bits);
        return false;
    }

    hda_flush();

    stream_rate = rate;
    stream_bits = bits;
    stream_format = hda_encode_format(stream_rate, stream_bits, stream_channels);
    codec_command(VERB16(info.codec_address, info.dac_node, VERB_SET_FORMAT,
                         stream_format), NULL);

    if (!setup_stream()) {
        kerr("hda", "the stream would not come back after the format change");
        return false;
    }

    info.sample_rate = stream_rate;
    info.bits = stream_bits;
    kinfo("hda", "output now %u.%u kHz at %u bits, as asked",
          stream_rate / 1000, (stream_rate % 1000) / 100, stream_bits);
    return true;
}

/* --------------------------------------------------------------- recording
 *
 * The capture stream is set up beside the playback one and was never started,
 * and nothing could read from it.  So the driver found the input converter,
 * the pin it listens on and the rates it accepts, reported all of that - and
 * a program asking to record got nothing, because the read slot on the device
 * was empty.  Detected, described, and impossible to use.
 *
 * Recording is the mirror of playing: the hardware writes into the ring and
 * software reads behind it, where playing has software write and the hardware
 * read.  The position register means the same thing in both directions - how
 * far the hardware has got - so what is available to a reader is everything
 * between where it last read and where the hardware has written to.
 */
static u32 capture_read_pos;
static bool capture_running;

static void capture_start(void) {
    if (!capture_ready || capture_running) return;

    /* Start from where the hardware is, not from zero: the buffer holds
     * whatever was in memory before, and handing that to a caller as though it
     * were sound is worse than a moment of silence. */
    capture_read_pos = rd32(capture_base + SD_LPIB) % BUFFER_BYTES;

    wr8(capture_base + SD_CTL,
        (u8)(rd8(capture_base + SD_CTL) | SDCTL_RUN));
    capture_running = true;
}

void hda_capture_stop(void) {
    if (!capture_ready || !capture_running) return;
    wr8(capture_base + SD_CTL,
        (u8)(rd8(capture_base + SD_CTL) & ~SDCTL_RUN));
    capture_running = false;
}

/* How much has arrived and not yet been taken. */
int hda_recorded(void) {
    if (!capture_ready || !capture_running) return 0;
    u32 head = rd32(capture_base + SD_LPIB) % BUFFER_BYTES;
    return (int)((head - capture_read_pos) % BUFFER_BYTES);
}

/* Take bytes out of the ring, wrapping if the run crosses the end.
 *
 * Separated from the register reading so it can be checked: everything above
 * this line depends on hardware, and this part is arithmetic that produces
 * plausible audio when it is wrong.  A copy that runs off the end without
 * coming back to the beginning yields whatever follows the buffer in memory,
 * which sounds like noise rather than like an error.
 */
u32 capture_take(u8 *dst, const u8 *ring, u32 ring_bytes, u32 read_pos,
                 u32 bytes) {
    if (!dst || !ring || !ring_bytes || !bytes) return read_pos;
    if (bytes > ring_bytes) bytes = ring_bytes;

    u32 first = ring_bytes - read_pos;
    if (first > bytes) first = bytes;

    memcpy(dst, ring + read_pos, first);
    if (bytes > first) memcpy(dst + first, ring, bytes - first);

    return (read_pos + bytes) % ring_bytes;
}

int hda_read(void *samples, int bytes) {
    if (!capture_ready || bytes <= 0) return 0;

    capture_start();

    int have = hda_recorded();

    /* Whether the writer has been round and past us.
     *
     * The distance from the reader to the hardware is computed modulo the
     * buffer, so a reader that has fallen a whole lap behind sees a *small*
     * number rather than a huge one - the lap is invisible, and what comes
     * back is a moment from some time ago presented as the present.  Nothing
     * reports it, because from the arithmetic's point of view nothing is
     * wrong.
     *
     * Nearly a full buffer of unread audio means that has happened, or is
     * about to.  Beginning again just behind the hardware loses what was
     * missed, which has already been lost - the difference is that this way
     * the reader knows where it is.
     */
    if (have > (int)(BUFFER_BYTES - BUFFER_BYTES / 4)) {
        kwarn("hda", "recording fell behind and the buffer wrapped past it; "
                     "starting again from where the hardware is");
        capture_read_pos = (rd32(capture_base + SD_LPIB) % BUFFER_BYTES);
        return 0;
    }

    if (have <= 0) return 0;
    if (bytes > have) bytes = have;

    /* One segment back from the hardware, so a reader never takes bytes the
     * controller is in the middle of writing. */
    int margin = SEGMENT_BYTES;
    if (bytes > have - margin) bytes = have - margin;
    if (bytes <= 0) return 0;

    capture_read_pos = capture_take(samples, capture_buffer, BUFFER_BYTES,
                                    capture_read_pos, (u32)bytes);
    return bytes;
}

/* Check the wrapping, which is the half of recording that needs no hardware.
 *
 * A run that crosses the end of the ring is the case that goes wrong, and it
 * goes wrong quietly: the bytes that come back are still bytes and still play.
 */
int hda_capture_selftest(void) {
    int failures = 0;

    enum { RING = 256 };
    static u8 ring[RING];
    static u8 got[RING];

    for (u32 i = 0; i < RING; i++) ring[i] = (u8)i;

    /* Every starting position, and a run long enough to cross the end from
     * most of them. */
    for (u32 start = 0; start < RING; start += 7) {
        const u32 take = 100;
        memset(got, 0, sizeof got);

        u32 after = capture_take(got, ring, RING, start, take);

        if (after != (start + take) % RING) {
            kwarn("hda", "selftest: reading %u from %u left the position at "
                         "%u, expected %u", take, start, after,
                  (start + take) % RING);
            failures++;
            break;
        }

        for (u32 i = 0; i < take; i++) {
            u8 want = (u8)((start + i) % RING);
            if (got[i] == want) continue;
            kwarn("hda", "selftest: reading %u from %u gave %#x at %u, "
                         "expected %#x - the wrap is wrong",
                  take, start, got[i], i, want);
            failures++;
            break;
        }
        if (failures) break;
    }

    if (!failures)
        kinfo("hda", "recording returns the right bytes from every position, "
                     "including the runs that cross the end of the ring");
    return failures;
}

/* ------------------------------------------------------------- /dev/audio */

/* Two nodes, one write path.  The context pointer is what says which device a
 * caller asked for: /dev/audio is the machine's own, and /dev/usbaudio is
 * whatever is plugged in.  Distinguishing them by the pointer rather than by a
 * flag means neither can be opened and then quietly redirected. */
static const char usb_audio_marker;

static ssize_t_k audio_dev_write(void *ctx, const void *buf, size_t len, u64 off) {
    (void)off;
    if (!len) return 0;
    if (len > (~0ull >> 1)) return -E_INVAL;

    /* A machine with no codec on its own controller is not a machine with
     * no sound: a USB headset is the only output on plenty of desks, and
     * refusing the write because the built-in part is absent would mean
     * nothing plays on exactly those machines. */
    /* Which one this write is for.  A machine with a codec of its own and a
     * headset plugged in has two, and until now the built-in one always won -
     * so a USB headset on a desktop was driven correctly and could not be
     * played through, which is the same as not working. */
    bool use_usb = (ctx == &usb_audio_marker) || !ready;
    if (use_usb && !usbaudio_can_play()) return -E_NODEV;

    /* Block until the ring has room rather than dropping samples: a program
     * writing a stream expects the write to pace it. */
    size_t written = 0;
    while (written < len) {
        if (proc_stop_requested()) return written ? (ssize_t_k)written : -E_INTR;
        if (use_usb ? !usbaudio_can_play() : !ready)
            return written ? (ssize_t_k)written : -E_NODEV;
        size_t request = len - written;
        if (request > 0x7fffffffu) request = 0x7fffffffu;
        int n = use_usb
                    ? usbaudio_write((const u8 *)buf + written, (int)request)
                    : hda_write((const u8 *)buf + written, (int)request);
        if (n < 0) return written ? (ssize_t_k)written : n;
        if ((size_t)n > request) return written ? (ssize_t_k)written : -E_IO;
        if (n > 0) { written += (size_t)n; continue; }
        sched_sleep_ms(5);
    }
    return (ssize_t_k)written;
}

/* Mute or restore the active output path.  Muting sets the amplifier mute bit
 * on both the converter and the pin (and drops the gain to zero as belt and
 * braces); restoring puts each back to its full gain.  Independent of the
 * volume slider, which scales the samples the software mixes. */
static bool set_output_mute(bool mute) {
    /* Codec address zero is valid, and is common on real and emulated HDA. */
    if (!ready || !dac_node) return false;
    u8 dg = mute ? 0 : max_gain(sel_codec, dac_node, true);
    if (!codec_command(VERB16(sel_codec, dac_node, VERB_SET_AMP_GAIN,
                         (u16)(0x8000 | 0x3000 | (mute ? 0x80 : 0) | dg)), NULL))
        return false;
    if (out_pin_node) {
        u8 pg = mute ? 0 : max_gain(sel_codec, out_pin_node, true);
        if (!codec_command(VERB16(sel_codec, out_pin_node, VERB_SET_AMP_GAIN,
                             (u16)(0x8000 | 0x3000 | (mute ? 0x80 : 0) | pg)), NULL))
            return false;
    }
    out_muted = mute;
    return true;
}

static int audio_dev_ioctl(void *ctx, u32 cmd, void *arg) {
    /* A caller asking what format to write in must be answered for the device
     * it opened, not for the other one. */
    if (ctx == &usb_audio_marker && cmd == 3) {
        if (!arg) return -E_INVAL;
        u32 *out = arg;
        /* Capabilities can exceed the raw sample format currently admitted.
         * Report the opened playback format, not the highest advertised one. */
        if (!usbaudio_playback_format(&out[0], &out[1], &out[2])) return -E_NODEV;
        return 0;
    }

    switch (cmd) {
    case 1:                                   /* bytes of room               */
        if (arg) *(u32 *)arg = (u32)hda_space();
        return 0;
    case 2:                                   /* stop and discard            */
        hda_flush();
        return 0;
    case 3:                                   /* sample rate, channels, bits */
        if (arg) {
            u32 *out = arg;
            out[0] = info.sample_rate;
            out[1] = info.channels;
            out[2] = info.bits;
        }
        return 0;
    case 4:                                   /* bytes written, not yet played */
        if (arg) *(u32 *)arg = (u32)hda_queued();
        return 0;
    case 5:                                   /* where the hardware has read to */
        if (arg) *(u32 *)arg = hda_position();
        return 0;

    /* The formats on offer, and choosing one.  Slots rather than a struct so
     * the caller side needs no shared header beyond "an array of words":
     *   out[0]        how many rates follow
     *   out[1..12]    the rates, slowest first, as the converter listed them
     *   out[13]       how many depths follow
     *   out[14..18]   the depths in bits
     */
    case 8: {
        if (!arg) return -E_INVAL;
        u32 *out = arg;
        u32 rates[12];
        u8  depths[5];
        int nd = 0;
        int nr = hda_formats(rates, 12, depths, 5, &nd);
        out[0] = (u32)nr;
        for (int i = 0; i < nr; i++) out[1 + i] = rates[i];
        out[13] = (u32)nd;
        for (int i = 0; i < nd; i++) out[14 + i] = depths[i];
        return 0;
    }
    case 9: {                                 /* set: {rate, bits}           */
        if (!arg) return -E_INVAL;
        u32 *in = arg;
        return hda_set_format(in[0], (u8)in[1]) ? 0 : -E_INVAL;
    }

    /* The output and input pins the codec offers, and choosing one.  Slots so
     * the caller needs no shared header:
     *   out[0]            how many endpoints follow
     *   out[1 + i*4 + 0]  node id
     *            + 1      device type (0 line-out, 1 speaker, 2 headphone,
     *                     8 line-in, 0xA mic, ... - the HDA default-device code)
     *            + 2      1 if an output, 0 if an input
     *            + 3      1 if it is the one currently in use
     *   out[1 + n*4]      the current output mute state (1 muted)
     */
    case 10: {
        if (!arg) return -E_INVAL;
        u32 *out = arg;
        int n = endpoint_count;
        out[0] = (u32)n;
        for (int i = 0; i < n; i++) {
            u32 *e = &out[1 + i * 4];
            e[0] = endpoints[i].node;
            e[1] = endpoints[i].device;
            e[2] = endpoints[i].is_output;
            e[3] = endpoints[i].is_output ? (endpoints[i].node == out_pin_node)
                                          : (endpoints[i].node == in_pin_node);
        }
        out[1 + n * 4] = out_muted ? 1 : 0;
        return 0;
    }
    case 11: {                                /* choose output pin {node}    */
        if (!arg) return -E_INVAL;
        u8 node = (u8)*(u32 *)arg;
        for (int i = 0; i < endpoint_count; i++)
            if (endpoints[i].is_output && endpoints[i].node == node) {
                bool muted = out_muted;
                if (!activate_output(sel_codec, node, false)) return -E_INVAL;
                return set_output_mute(muted) ? 0 : -E_IO;
            }
        return -E_INVAL;                       /* not a pin we offered */
    }
    case 12: {                                /* choose input pin {node}     */
        if (!arg) return -E_INVAL;
        u8 node = (u8)*(u32 *)arg;
        for (int i = 0; i < endpoint_count; i++)
            if (!endpoints[i].is_output && endpoints[i].node == node)
                return activate_input(sel_codec, node, afg_first, afg_total)
                           ? 0 : -E_INVAL;
        return -E_INVAL;
    }
    case 13:                                  /* mute output {1|0}           */
        if (!arg) return -E_INVAL;
        return set_output_mute(*(u32 *)arg != 0) ? 0 : -E_IO;

    default:
        return -E_INVAL;
    }
}

static u64 audio_dev_size(void *ctx) { (void)ctx; return 0; }

static ssize_t_k audio_dev_read(void *ctx, void *buf, size_t len, u64 off) {
    (void)ctx; (void)off;
    if (!len) return 0;
    if (!capture_ready) return -E_NODEV;
    size_t request = len < 0x7fffffffu ? len : 0x7fffffffu;

    /* Block until something has arrived, the way the write side blocks until
     * there is room: a reader asking for sound expects the read to pace it
     * rather than to spin returning nothing. */
    for (int i = 0; i < 400; i++) {
        if (proc_stop_requested()) return -E_INTR;
        if (!capture_ready) return -E_NODEV;
        int n = hda_read(buf, (int)request);
        if (n < 0) return n;
        if ((size_t)n > request) return -E_IO;
        if (n > 0) return (ssize_t_k)n;
        sched_sleep_ms(5);
    }
    return 0;
}

static int audio_dev_ioctl_shape(void *ctx, u32 cmd, vfs_ioctl_shape_t *shape) {
    *shape = (vfs_ioctl_shape_t){0};
    switch (cmd) {
    case 1: case 4: case 5: shape->out_bytes = 4; break;
    case 2: break;
    case 3:
        shape->out_bytes = 12;
        shape->required = ctx == &usb_audio_marker;
        break;
    case 8: shape->out_bytes = 19 * sizeof(u32); shape->required = true; break;
    case 9: shape->in_bytes = 2 * sizeof(u32); shape->required = true; break;
    case 10:
        /* Fixed capacity, including count + mute. Users supply space for all
         * endpoints; zero-filled unused entries do not disclose kernel bytes. */
        shape->out_bytes = (2 + 4 * (sizeof endpoints / sizeof endpoints[0])) * sizeof(u32);
        shape->required = true;
        break;
    case 11: case 12: case 13: shape->in_bytes = 4; shape->required = true; break;
    default: return -E_INVAL;
    }
    return 0;
}

static const devfs_ops_t audio_ops = {
    .read = audio_dev_read, .write = audio_dev_write,
    .ioctl = audio_dev_ioctl, .size = audio_dev_size,
    .ioctl_shape = audio_dev_ioctl_shape,
};

/* --------------------------------------------------------------------- init */

/* Try one controller.  Returns true when it ended up with an output that can
 * actually be played through. */
static bool hda_try(pci_dev_t *d) {
    memset(&info, 0, sizeof info);

    info.pci_vendor = d->vendor;
    info.pci_device = d->device;
    snprintf(info.controller, sizeof info.controller, "%s HD Audio",
             pci_vendor_name(d->vendor));

    if (!d->bar[0] || d->bar_is_io[0]) {
        kwarn("hda", "the audio controller has no memory window");
        return false;
    }

    pci_enable_memory(d);
    pci_enable_bus_master(d);

    size_t len = d->bar_size[0] ? (size_t)d->bar_size[0] : 0x4000;
    if (len > 0x10000) len = 0x10000;
    regs = vmm_map_mmio(d->bar[0], len);
    if (!regs) {
        kerr("hda", "cannot map the audio controller registers");
        return false;
    }

    info.present = true;

    if (!reset_controller()) return false;

    u32 gcap = rd16(GCAP);
    u8 in_streams = (u8)((gcap >> 8) & 0x0F);
    in_streams_count = in_streams;
    u8 out_streams = (u8)((gcap >> 12) & 0x0F);
    if (!out_streams) {
        kwarn("hda", "the controller reports no output streams");
        strlcpy(info.note, "the controller has no output stream", sizeof info.note);
        return false;
    }
    /* Output stream descriptors follow the input ones. */
    out_stream_base = SD_BASE + (u32)in_streams * SD_STRIDE;

    if (!setup_rings()) {
        kerr("hda", "out of memory setting up the command rings");
        return false;
    }

    u16 present_codecs = rd16(STATESTS);
    if (!present_codecs) {
        kwarn("hda", "no codec answered the reset");
        strlcpy(info.note, "no codec answered", sizeof info.note);
        return false;
    }

    bool configured = false;
    for (u8 codec = 0; codec < 15 && !configured; codec++) {
        if (!(present_codecs & (1u << codec))) continue;
        configured = configure_codec(codec);
        if (!configured)
            kdebug("hda", "codec %u has no usable output path", codec);
    }

    if (!configured) {
        kwarn("hda", "no codec offered an output this driver can drive");
        strlcpy(info.note, "a codec answered, but with no output path",
                sizeof info.note);
        return false;
    }

    if (!setup_stream()) {
        kerr("hda", "out of memory setting up the output stream");
        return false;
    }

    /* And the recording side, when the codec offered one.  A machine with no
     * input is not a failure and does not stop anything above. */
    if (adc_node && setup_capture_stream()) {
        capture_ready = true;
        info.input_ready = true;
        info.input_rate = capture_rate;
        info.input_bits = capture_bits;
        info.input_channels = capture_channels;
        info.adc_node = adc_node;
        info.in_pin_node = in_pin_node;
        kinfo("hda", "recording: node %u from pin %u at %u Hz %u-bit - the "
                     "highest that converter reported",
              adc_node, in_pin_node, capture_rate, capture_bits);
    } else if (adc_node) {
        kwarn("hda", "a recording path was found but its stream would not "
                     "start; recording is unavailable");
    } else {
        kinfo("hda", "this codec offers no recording input");
    }

    info.output_ready = true;
    info.sample_rate = stream_rate;
    info.channels = stream_channels;
    info.bits = stream_bits;
    ready = true;

    u16 codec_vendor = (u16)(info.codec_vendor_id >> 16);
    const char *name = codec_vendor_name(codec_vendor);
    if (name)
        snprintf(info.codec, sizeof info.codec, "%s %04x", name,
                 (u16)info.codec_vendor_id);
    else
        snprintf(info.codec, sizeof info.codec, "codec %08x", info.codec_vendor_id);

    snprintf(info.note, sizeof info.note,
             "%u.%u kHz %u-bit stereo output, the best this codec offers",
             stream_rate / 1000, (stream_rate % 1000) / 100, stream_bits);

    pci_claim(d, "hda");

    devfs_register("audio", VN_CHR, &audio_ops, NULL);

    /* And the same operations again, carrying the marker, so that a program
     * can reach a plugged-in device on a machine that also has its own. */
    devfs_register("usbaudio", VN_CHR, &audio_ops, (void *)&usb_audio_marker);

    kinfo("hda", "%s: %s, node %u out to pin %u, %u Hz %u-bit stereo on "
                 "/dev/audio - the highest this codec reported",
          info.controller, info.codec, info.dac_node, info.pin_node,
          stream_rate, stream_bits);
    return true;
}


/* Every HD Audio controller in the machine, not just the first.
 *
 * A desktop with a graphics card has at least two: the one on the motherboard
 * that the speakers and the headphone socket are wired to, and one on the card
 * for audio over the display cable.  They are the same class of device and
 * enumeration returns them in bus order, so taking the first meant taking
 * whichever happened to sit lower - and a machine whose first controller has
 * no usable output got silence, with a working codec sitting beside it
 * untouched.
 *
 * So each is tried until one yields an output.  A controller that offers only
 * display audio is a real answer when it is the only one, and the wrong answer
 * when it is not.
 */
void hda_init(void) {
    pci_dev_t *d = NULL;
    int seen = 0;

    while ((d = pci_find(0x04, 0x03, 0xFF, d)) != NULL) {
        seen++;
        if (hda_try(d)) {
            if (seen > 1)
                kinfo("hda", "this is controller %d; the ones before it had no "
                             "output this driver could use", seen);
            return;
        }
        kdebug("hda", "%02x:%02x.%u offered no usable output; trying the next",
               d->bus, d->slot, d->func);
    }

    if (!seen) kdebug("hda", "no HD Audio controller present");
    else kwarn("hda", "%d HD Audio controller(s), none with an output this "
                      "driver can drive", seen);
}
