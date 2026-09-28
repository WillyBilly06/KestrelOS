/* usbaudio.c - a USB audio device: what it is and what it can do.
 *
 * A USB headset is not one device but four or five interfaces that describe
 * each other.  One of them is the control interface, which owns the clock and
 * the volume; the others are streaming interfaces, one per direction, and each
 * of those has several alternate settings that differ only in the format they
 * carry.  A device offering sixteen, twenty-four and thirty-two bit recording
 * does it by having three alternates on its input interface, and choosing the
 * best one means reading all three and picking.
 *
 * That last part is the point of this file.  The system's built-in audio was
 * for a long time driven at a format chosen in the driver rather than asked of
 * the device, and the same mistake here would drive a headset capable of
 * twenty-four bits at sixteen and never say so.
 *
 * What this does and does not do:
 *
 *   It reads the device's own description and reports what it can do - the
 *   directions, the channel counts, the sample sizes, and which alternate
 *   setting carries each.  That is what a system information window should
 *   show, and what a later streaming path has to choose from.
 *
 *   It opens the endpoints those settings live on.  They are isochronous,
 *   which is a different kind of transfer from bulk and interrupt: those are
 *   asked for and answered, while an isochronous endpoint holds a slot in
 *   every interval whether or not there is anything to put in it.  The host
 *   controller driver carries that now, and this driver names the endpoints
 *   worth reserving it for and selects the alternate setting each one needs.
 *
 *   It does not yet move audio.  An open endpoint carries nothing until
 *   something has samples to hand it, and the mixer that would is not written.
 *   So what this proves is that the device is understood and the bandwidth is
 *   reserved - not that sound comes out of it, which is worth saying plainly
 *   rather than leaving the device listed as driven.
 */
#include "kernel.h"
#include "klog.h"
#include "usb.h"

/* The class, and the two subclasses that matter. */
#define USB_CLASS_AUDIO        0x01
#define AUDIO_SUBCLASS_CONTROL 0x01
#define AUDIO_SUBCLASS_STREAM  0x02
#define AUDIO_PROTOCOL_V2      0x20

/* Descriptors that belong to the audio class rather than to USB itself. */
#define AUDIO_DT_INTERFACE     0x24
#define AUDIO_DT_ENDPOINT      0x25

/* The ones this reads. */
#define AS_GENERAL             0x01   /* what a streaming interface carries */
#define AS_FORMAT_TYPE         0x02   /* how many bits, and how many bytes  */
#define AC_CLOCK_SOURCE        0x0A   /* where its sample clock comes from  */

#define MAX_AUDIO_DEVICES 4
#define MAX_ALTERNATES    8

typedef struct {
    u8  interface;
    u8  alternate;
    u8  endpoint;
    bool input;              /* true when it records, false when it plays   */
    u8  channels;
    u8  bits;                /* 16, 24, 32                                  */
    u8  bytes_per_sample;
    u16 max_packet;
    u8  interval;            /* from the endpoint descriptor                */
    bool offered;            /* handed to the controller driver once        */
    bool opened;             /* and it accepted                             */
    bool fixed_48k;          /* UAC1 advertises exactly one rate, 48000 Hz  */
} audio_alt_t;

typedef struct {
    bool used;
    usb_device_t *dev;
    u8   control_interface;
    bool version2;

    audio_alt_t alt[MAX_ALTERNATES];
    int  alt_count;

    /* The best of each direction, which is what gets reported. */
    int  best_in, best_out;
    int  play_out;          /* supported active format, distinct from capability */
} audio_dev_t;

static audio_dev_t devices[MAX_AUDIO_DEVICES];

/* ------------------------------------------------------------- the walk
 *
 * The configuration descriptor is walked again here rather than reusing what
 * the interface binder found, because what matters for audio is exactly the
 * part the binder skips: the alternate settings other than zero, and the
 * class-specific descriptors between them.
 */
static bool audio_alt_valid(const audio_alt_t *alt, bool pcm) {
    return pcm && alt->endpoint && alt->channels && alt->bytes_per_sample &&
           alt->bytes_per_sample <= 4 && alt->bits &&
           alt->bits <= 8u * alt->bytes_per_sample && alt->max_packet &&
           alt->interval;
}

static void read_alternates(audio_dev_t *a, const u8 *cfg, int total) {
    u8 current_interface = 0xFF;
    u8 current_alt = 0;
    bool streaming = false;
    audio_alt_t pending;
    bool have_pending = false;
    bool version2 = false;
    bool pcm = false;

    memset(&pending, 0, sizeof pending);

    for (int off = 0; off + 2 <= total; ) {
        u8 len = cfg[off];
        u8 type = cfg[off + 1];
        if (len < 2 || off + len > total) break;

        if (type == USB_DT_INTERFACE && len >= 9) {
            /* The previous alternate is complete. */
            if (have_pending && audio_alt_valid(&pending, pcm) &&
                a->alt_count < MAX_ALTERNATES) {
                a->alt[a->alt_count++] = pending;
            }
            memset(&pending, 0, sizeof pending);
            have_pending = false;
            pcm = false;

            current_interface = cfg[off + 2];
            current_alt = cfg[off + 3];
            u8 cls = cfg[off + 5], sub = cfg[off + 6], proto = cfg[off + 7];

            version2 = (proto == AUDIO_PROTOCOL_V2);
            streaming = (cls == USB_CLASS_AUDIO && sub == AUDIO_SUBCLASS_STREAM &&
                         (proto == 0 || version2));
            if (cls == USB_CLASS_AUDIO && sub == AUDIO_SUBCLASS_CONTROL) {
                a->control_interface = current_interface;
                a->version2 = (proto == AUDIO_PROTOCOL_V2);
            }

            if (streaming && current_alt != 0) {
                pending.interface = current_interface;
                pending.alternate = current_alt;
                have_pending = true;
            }
        } else if (type == AUDIO_DT_INTERFACE && have_pending && len >= 3) {
            u8 subtype = cfg[off + 2];

            /* USB Audio 1.0 AS_GENERAL has wFormatTag, not channels;
             * FORMAT_TYPE_I carries channels/subframe/bits at 4/5/6.
             * UAC2 moves channels to AS_GENERAL[10] and uses 4/5 for
             * subslot/bits.  See Linux usb/audio.h and usb/audio-v2.h. */
            if (subtype == AS_GENERAL) {
                pcm = false;
                if (version2 && len >= 16) {
                    pcm = cfg[off + 5] == 1 && (cfg[off + 6] & 1);
                    pending.channels = cfg[off + 10];
                } else if (!version2 && len >= 7) {
                    pcm = cfg[off + 5] == 1 && cfg[off + 6] == 0;
                }
            } else if (subtype == AS_FORMAT_TYPE) {
                pending.bytes_per_sample = pending.bits = 0;
                pending.fixed_48k = false;
                if (version2 && len >= 6 && cfg[off + 3] == 1) {
                    pending.bytes_per_sample = cfg[off + 4];
                    pending.bits = cfg[off + 5];
                } else if (!version2 && len >= 8 && cfg[off + 3] == 1) {
                    /* A zero frequency count describes a lower/upper range;
                     * otherwise every declared 24-bit rate must be present. */
                    u8 rates = cfg[off + 7];
                    if (len >= (rates ? 8u + 3u * rates : 14u)) {
                        pending.channels = cfg[off + 4];
                        pending.bytes_per_sample = cfg[off + 5];
                        pending.bits = cfg[off + 6];
                        u32 first = (u32)cfg[off + 8] | ((u32)cfg[off + 9] << 8) |
                                    ((u32)cfg[off + 10] << 16);
                        pending.fixed_48k = rates == 1 && first == 48000;
                        if (!rates) {
                            u32 last = (u32)cfg[off + 11] | ((u32)cfg[off + 12] << 8) |
                                       ((u32)cfg[off + 13] << 16);
                            pending.fixed_48k = first == 48000 && last == 48000;
                        }
                    }
                }
            }
        } else if (type == USB_DT_ENDPOINT && have_pending && len >= 7) {
            u8 addr = cfg[off + 2];
            u8 attrs = cfg[off + 3];

            /* Explicit feedback is ALSO isochronous (usage bits 5:4 = 1),
             * but is not a sample endpoint.  Do not replace playback with its
             * feedback IN endpoint when it follows the data descriptor. */
            if ((attrs & USB_EP_XFER_MASK) == USB_EP_XFER_ISOC &&
                (attrs & 0x30u) != 0x10u && !pending.endpoint) {
                pending.endpoint = addr;
                pending.input = (addr & USB_DIR_IN) != 0;
                pending.max_packet = (u16)((cfg[off + 4] | (cfg[off + 5] << 8)) & 0x7FF);
                pending.interval = cfg[off + 6];
            }
        }
        off += len;
    }

    if (have_pending && audio_alt_valid(&pending, pcm) &&
        a->alt_count < MAX_ALTERNATES)
        a->alt[a->alt_count++] = pending;

    (void)current_alt;
}

/* Pick the best alternate in each direction.
 *
 * Best means the most bits, and then the most channels: a device that offers
 * sixteen and twenty-four bit recording is capable of twenty-four, and driving
 * it at sixteen because that was listed first throws away exactly the thing
 * somebody bought the device for. */
static void choose_best(audio_dev_t *a) {
    a->best_in = a->best_out = -1;
    a->play_out = -1;

    for (int i = 0; i < a->alt_count; i++) {
        /* The current writer supplies raw stereo s16/48k, with no resampler
         * or sample-container conversion. UAC2 clock controls and variable
         * UAC1 rates must be negotiated before they become playable. */
        const audio_alt_t *alt = &a->alt[i];
        if (a->play_out < 0 && !alt->input && alt->channels == 2 &&
            alt->bits == 16 && alt->bytes_per_sample == 2 && alt->fixed_48k)
            a->play_out = i;
        int *slot = a->alt[i].input ? &a->best_in : &a->best_out;
        if (*slot < 0) { *slot = i; continue; }

        const audio_alt_t *best = &a->alt[*slot];
        const audio_alt_t *here = &a->alt[i];

        if (here->bits > best->bits ||
            (here->bits == best->bits && here->channels > best->channels))
            *slot = i;
    }
}

/* ------------------------------------------------------------------ probe */

void *usbaudio_probe(usb_device_t *dev, const usb_interface_t *ifc,
                     const u8 *cfg, int cfg_len) {
    if (ifc->dev_class != USB_CLASS_AUDIO) return NULL;

    /* One entry per device, not per interface: a headset presents several
     * audio interfaces and they are one device. */
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++)
        if (devices[i].used && devices[i].dev == dev) return &devices[i];

    audio_dev_t *a = NULL;
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++)
        if (!devices[i].used) { a = &devices[i]; break; }
    if (!a) return NULL;

    memset(a, 0, sizeof *a);
    a->used = true;
    a->dev = dev;
    a->control_interface = 0xFF;

    read_alternates(a, cfg, cfg_len);
    choose_best(a);

    if (!a->alt_count) {
        kinfo("usbaudio", "%s: an audio device with no streaming settings this "
                          "driver understands", usb_device_name(dev));
        a->used = false;
        return NULL;
    }

    kinfo("usbaudio", "%s: USB audio%s, %d streaming setting(s)",
          usb_device_name(dev), a->version2 ? " 2.0" : "", a->alt_count);

    if (a->best_out >= 0) {
        const audio_alt_t *o = &a->alt[a->best_out];
        kinfo("usbaudio", "  playback: %u channel(s) at %u bits, on endpoint "
                          "%#x (interface %u setting %u) - the best of the %d "
                          "it offers",
              o->channels, o->bits, o->endpoint, o->interface, o->alternate,
              a->alt_count);
    }
    if (a->best_in >= 0) {
        const audio_alt_t *n = &a->alt[a->best_in];
        kinfo("usbaudio", "  recording: %u channel(s) at %u bits, on endpoint "
                          "%#x (interface %u setting %u)",
              n->channels, n->bits, n->endpoint, n->interface, n->alternate);
    }

    if (a->best_out >= 0 && a->play_out < 0)
        kwarn("usbaudio", "  playback capability found, but no fixed 48k stereo "
                          "s16 alternate; conversion/clock negotiation unavailable");
    else if (a->play_out >= 0)
        kinfo("usbaudio", "  selected playback: stereo s16 at fixed 48000 Hz, "
                          "interface %u setting %u (capabilities retained above)",
              a->alt[a->play_out].interface, a->alt[a->play_out].alternate);

    /* The endpoints are opened from the host controller driver, which calls
     * back for each one this chose. */
    return a;
}

/* ----------------------------------------------------- opening the endpoints
 *
 * The host controller driver configures endpoints; this driver knows which
 * ones are worth configuring.  Rather than one calling into the other's
 * internals, the choice is handed over one endpoint at a time.
 */
bool usbaudio_next_endpoint(void *ctx, u8 *addr, u16 *max_packet,
                            u8 *interval, u8 *burst,
                            u8 *interface, u8 *alternate) {
    audio_dev_t *a = ctx;
    if (!a || !a->used) return false;

    for (int pass = 0; pass < 2; pass++) {
        int which = pass ? a->best_in : a->play_out;
        if (which < 0) continue;

        audio_alt_t *alt = &a->alt[which];
        if (alt->opened || alt->offered) continue;

        alt->offered = true;
        *addr = alt->endpoint;
        *max_packet = alt->max_packet;
        /* One interval, which for these devices is every microframe.  The
         * descriptor's own figure is used where it gave one. */
        *interval = alt->interval ? alt->interval : 1;
        *burst = 0;
        /* Which setting has to be selected for this endpoint to exist at all.
         * Alternate zero of a streaming interface deliberately has no
         * endpoints - that is how a device says "not in use" and gives its
         * bandwidth back - so the endpoint is only real once this is set. */
        *interface = alt->interface;
        *alternate = alt->alternate;
        return true;
    }
    return false;
}

void usbaudio_endpoint_open(void *ctx, u8 addr, bool ok) {
    audio_dev_t *a = ctx;
    if (!a || !a->used) return;

    for (int i = 0; i < a->alt_count; i++) {
        /* Alternates commonly reuse an address.  Only the selected alternate
         * handed to the controller can have completed this open request. */
        if (a->alt[i].endpoint != addr || !a->alt[i].offered) continue;
        a->alt[i].opened = ok;

        if (ok)
            kinfo("usbaudio", "  %s endpoint %#x is open at %u bits",
                  a->alt[i].input ? "recording" : "playback", addr,
                  a->alt[i].bits);
        else
            kwarn("usbaudio", "  %s endpoint %#x would not open",
                  a->alt[i].input ? "recording" : "playback", addr);
        return;
    }
}

/* ------------------------------------------------------------ the sample ring
 *
 * Between a program writing sound and a controller that must have something to
 * send every 125 microseconds there has to be a buffer, and its size is a
 * judgement: too small and any pause in the writer is heard as a gap, too
 * large and what the writer asks for is heard a noticeable time later.
 *
 * 64 KiB is about 170 ms of 24-bit stereo at 96 kHz.  Long enough to ride out
 * a scheduler hiccup, short enough that nobody notices the delay.
 */
#define RING_BYTES 65536

typedef struct {
    u8 buf[RING_BYTES];
    u32 head, tail;
    bool running;
} audio_ring_t;
static audio_ring_t playback_ring;

static audio_dev_t *audio_playback_device(void) {
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++) {
        audio_dev_t *a = &devices[i];
        if (!a->used || a->play_out < 0 || a->play_out >= a->alt_count) continue;
        const audio_alt_t *alt = &a->alt[a->play_out];
        if (alt->opened && !alt->input && alt->channels == 2 && alt->bits == 16 &&
            alt->bytes_per_sample == 2 && alt->fixed_48k)
            return a;
    }
    return NULL;
}

static u32 ring_used(const audio_ring_t *ring) { return ring->head - ring->tail; }

int usbaudio_space(void) {
    return (int)(RING_BYTES - ring_used(&playback_ring));
}

static int audio_ring_write(audio_ring_t *ring, const void *samples, int bytes) {
    if (bytes <= 0 || !samples) return 0;

    u32 room = RING_BYTES - ring_used(ring);
    if (!room) return 0;
    if ((u32)bytes > room) bytes = (int)room;

    const u8 *in = samples;
    for (int i = 0; i < bytes; i++)
        ring->buf[(ring->head + (u32)i) % RING_BYTES] = in[i];
    ring->head += (u32)bytes;
    ring->running = true;
    return bytes;
}

int usbaudio_write(const void *samples, int bytes) {
    if (!audio_playback_device()) return 0;
    return audio_ring_write(&playback_ring, samples, bytes);
}

/* One interval's worth, handed to the controller.
 *
 * Returning zero means "nothing to send", and for an isochronous endpoint that
 * is a real answer rather than an error: the slot passes unused and the next
 * one comes round in 125 microseconds.  What must not happen is returning
 * silence forever, which would keep the endpoint busy and the card awake long
 * after anything had anything to play.
 */
static u32 audio_ring_take(audio_ring_t *ring, void *buf, u32 max) {
    if (!ring->running || !buf) return 0;
    u32 have = ring_used(ring);
    if (!have) { ring->running = false; return 0; }
    if (have > max) have = max;
    have &= ~3u; /* whole interleaved stereo s16 frames; retain partial writes */

    u8 *out = buf;
    for (u32 i = 0; i < have; i++)
        out[i] = ring->buf[(ring->tail + i) % RING_BYTES];
    ring->tail += have;
    return have;
}

u32 usbaudio_next_samples(void *ctx, u8 addr, void *buf, u32 max) {
    audio_dev_t *a = ctx;
    if (!a || a != audio_playback_device() || a->alt[a->play_out].endpoint != addr)
        return 0;
    return audio_ring_take(&playback_ring, buf, max);
}

/* Whether anything can be played at all: an endpoint that was offered, opened,
 * and is a playback one. */
bool usbaudio_can_play(void) {
    return audio_playback_device() != NULL;
}

bool usbaudio_playback_format(u32 *rate, u32 *channels, u32 *bits) {
    if (!audio_playback_device()) return false;
    if (rate) *rate = 48000;
    if (channels) *channels = 2;
    if (bits) *bits = 16;
    return true;
}

/* ------------------------------------------------------------------- tests
 *
 * A ring between a writer and something reading a fixed amount every interval
 * is arithmetic that fails quietly.  Bytes taken from the wrong offset are
 * still bytes and still play - as a click, or as a fragment of a moment ago -
 * and nothing reports an error, because from the ring's point of view nothing
 * went wrong.  The wrap is where it happens: a copy that runs off the end
 * without coming back to the beginning loses whatever crossed the boundary.
 *
 * So what is checked is that what comes out is what went in, byte for byte,
 * across the wrap and several times over.
 */
int usbaudio_ring_selftest(void) {
    int failures = 0;

    /* Same ring implementation, separate storage: the USB thread must never
     * drain test bytes to a live headset or steal them from this comparison. */
    static audio_ring_t test_ring;
    test_ring.head = test_ring.tail = 0;
    test_ring.running = false;

    /* Enough passes that the ring wraps several times, with a chunk size that
     * does not divide the ring - so the wrap lands at a different offset every
     * time rather than always at the same convenient place. */
    enum { CHUNK = 3000 };
    static u8 out_buf[CHUNK];
    static u8 in_buf[CHUNK];

    u32 counter = 0;
    for (int pass = 0; pass < 64; pass++) {
        for (int i = 0; i < CHUNK; i++) out_buf[i] = (u8)(counter + i);

        int wrote = audio_ring_write(&test_ring, out_buf, CHUNK);
        if (wrote != CHUNK) {
            kwarn("usbaudio", "selftest: pass %d wrote %d of %d bytes",
                  pass, wrote, CHUNK);
            failures++;
            break;
        }

        u32 got = audio_ring_take(&test_ring, in_buf, CHUNK);
        if (got != CHUNK) {
            kwarn("usbaudio", "selftest: pass %d took %u of %d bytes",
                  pass, got, CHUNK);
            failures++;
            break;
        }

        for (int i = 0; i < CHUNK; i++) {
            if (in_buf[i] == out_buf[i]) continue;
            kwarn("usbaudio", "selftest: pass %d byte %d came back %#x, "
                              "expected %#x - the ring wrapped wrongly",
                  pass, i, in_buf[i], out_buf[i]);
            failures++;
            break;
        }
        if (failures) break;

        counter += CHUNK;
    }

    /* An empty ring hands back nothing rather than stale bytes, which is the
     * difference between silence and a fragment of a moment ago repeating. */
    if (!failures) {
        test_ring.running = true;
        u32 got = audio_ring_take(&test_ring, in_buf, CHUNK);
        if (got != 0) {
            kwarn("usbaudio", "selftest: an empty ring handed back %u bytes",
                  got);
            failures++;
        }
    }

    /* And it refuses more than it can hold rather than overwriting what has
     * not been played yet. */
    if (!failures) {
        test_ring.head = test_ring.tail = 0;
        int total = 0;
        for (int i = 0; i < 64; i++) {
            int n = audio_ring_write(&test_ring, out_buf, CHUNK);
            total += n;
            if (n < CHUNK) break;
        }
        if (total > RING_BYTES) {
            kwarn("usbaudio", "selftest: the ring took %d bytes into %u",
                  total, (unsigned)RING_BYTES);
            failures++;
        }
    }

    test_ring.head = test_ring.tail = 0;
    test_ring.running = false;

    if (!failures)
        kinfo("usbaudio", "the sample ring returns what was put into it, "
                          "across the wrap and when full");
    return failures;
}

/* ------------------------------------------------------------- reporting */

int usbaudio_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++) if (devices[i].used) n++;
    return n;
}

bool usbaudio_get(int index, char *name, size_t name_cap,
                  u32 *out_channels, u32 *out_bits,
                  u32 *in_channels, u32 *in_bits) {
    int n = 0;
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++) {
        if (!devices[i].used) continue;
        if (n++ != index) continue;

        audio_dev_t *a = &devices[i];
        if (name) strlcpy(name, usb_device_name(a->dev), name_cap);

        if (out_channels) *out_channels = a->best_out >= 0 ? a->alt[a->best_out].channels : 0;
        if (out_bits)     *out_bits     = a->best_out >= 0 ? a->alt[a->best_out].bits : 0;
        if (in_channels)  *in_channels  = a->best_in >= 0 ? a->alt[a->best_in].channels : 0;
        if (in_bits)      *in_bits      = a->best_in >= 0 ? a->alt[a->best_in].bits : 0;
        return true;
    }
    return false;
}
