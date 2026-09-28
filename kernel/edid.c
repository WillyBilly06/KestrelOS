/* edid.c - parse a monitor's EDID base block.  See edid.h.
 * Structure per VESA E-EDID; verified against real panels in edid_host_test.c. */
#ifdef EDID_HOST_TEST
#include <string.h>
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
#include "edid.h"
#include "edid_cea.h"
#else
#include "edid.h"
#include "edid_cea.h"
#include "klog.h"
#endif

static const u8 edid_magic[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

/* An 18-byte descriptor whose first three bytes are zero is a monitor
 * descriptor (name, range, serial); otherwise it is a detailed timing. */
static int is_detailed_timing(const u8 *d) {
    return !(d[0] == 0 && d[1] == 0 && d[2] == 0);
}

static void decode_detailed(const u8 *d, edid_mode_t *m) {
    u32 pclk_khz = (((u32)d[1] << 8) | d[0]) * 10u;    /* 10 kHz units -> kHz */
    u16 hact = (u16)((((u16)(d[4] >> 4)) << 8) | d[2]);
    u16 hbl  = (u16)((((u16)(d[4] & 0x0F)) << 8) | d[3]);
    u16 vact = (u16)((((u16)(d[7] >> 4)) << 8) | d[5]);
    u16 vbl  = (u16)((((u16)(d[7] & 0x0F)) << 8) | d[6]);

    /* Sync offsets and widths span bytes 8-11: each low byte plus 2 high bits
     * packed into byte 11 (hoff[9:8], hw[9:8], voff[5:4], vw[5:4]). */
    u16 hsoff = (u16)(d[8] | (((u16)(d[11] >> 6) & 0x3) << 8));
    u16 hsw   = (u16)(d[9] | (((u16)(d[11] >> 4) & 0x3) << 8));
    u16 vsoff = (u16)(((d[10] >> 4) & 0xF) | (((u16)(d[11] >> 2) & 0x3) << 4));
    u16 vsw   = (u16)((d[10] & 0xF) | (((u16)(d[11] >> 0) & 0x3) << 4));

    m->pixel_clock_khz = pclk_khz;
    m->hactive = hact;
    m->vactive = vact;
    m->htotal = (u16)(hact + hbl);
    m->vtotal = (u16)(vact + vbl);
    m->hsync_off = hsoff;
    m->hsync_w = hsw;
    m->vsync_off = vsoff;
    m->vsync_w = vsw;
    m->flags = d[17];

    u32 total = (u32)m->htotal * (u32)m->vtotal;
    /* refresh in millihertz = pixel_clock_hz * 1000 / total_pixels */
    m->refresh_mhz = total ? (u32)(((u64)pclk_khz * 1000u * 1000u) / total) : 0;
}

/* DisplayID 1.x Type-I detailed timing descriptor (20 bytes).  Unlike an EDID
 * DTD it carries a 24-bit pixel clock, which is where modern 1440p/240 and
 * similar high-bandwidth modes are commonly advertised.  Layout and +1 units
 * match NVIDIA 595's displayid.h / parseDisplayIdTiming1Descriptor(). */
static int decode_displayid_type1(const u8 *d, edid_mode_t *m) {
    memset(m, 0, sizeof *m);
    m->pixel_clock_khz =
        ((u32)d[0] | ((u32)d[1] << 8) | ((u32)d[2] << 16)) * 10u + 10u;
    u8 options = d[3];
    m->hactive = (u16)((u32)d[4] | ((u32)d[5] << 8)); m->hactive++;
    u16 hblank = (u16)((u32)d[6] | ((u32)d[7] << 8)); hblank++;
    m->hsync_off = (u16)((u32)d[8] | (((u32)d[9] & 0x7Fu) << 8)); m->hsync_off++;
    m->hsync_w = (u16)((u32)d[10] | ((u32)d[11] << 8)); m->hsync_w++;
    m->vactive = (u16)((u32)d[12] | ((u32)d[13] << 8)); m->vactive++;
    u16 vblank = (u16)((u32)d[14] | ((u32)d[15] << 8)); vblank++;
    m->vsync_off = (u16)((u32)d[16] | (((u32)d[17] & 0x7Fu) << 8)); m->vsync_off++;
    m->vsync_w = (u16)((u32)d[18] | ((u32)d[19] << 8)); m->vsync_w++;
    m->htotal = (u16)(m->hactive + hblank);
    m->vtotal = (u16)(m->vactive + vblank);
    if (m->htotal < m->hactive + m->hsync_off + m->hsync_w ||
        m->vtotal < m->vactive + m->vsync_off + m->vsync_w)
        return 0;
    /* Convert DisplayID polarity/interlace into the EDID DTD flag convention
     * consumed by the CA7D head programmer. */
    m->flags = 0x18u;
    if (d[9] & 0x80u) m->flags |= 0x02u;
    if (d[17] & 0x80u) m->flags |= 0x04u;
    if (options & 0x10u) m->flags |= 0x80u;
    u32 total = (u32)m->htotal * (u32)m->vtotal;
    m->refresh_mhz = total ?
        (u32)(((u64)m->pixel_clock_khz * 1000u * 1000u) / total) : 0;
    return 1;
}

/* DisplayID 2.0 Type-VII is the modern exact-timing form used when a timing no
 * longer fits the old 18-byte EDID DTD (large rasters/high clocks).  Its 20-byte
 * layout matches Type-I, but its pixel clock is in 1 kHz rather than 10 kHz
 * units.  This follows NVIDIA 595 parseDisplayId20Timing7Descriptor(). */
static int decode_displayid_type7(const u8 *d, edid_mode_t *m) {
    if (!decode_displayid_type1(d, m)) return 0;
    m->pixel_clock_khz =
        (u32)d[0] | ((u32)d[1] << 8) | ((u32)d[2] << 16);
    m->pixel_clock_khz++;
    u32 total = (u32)m->htotal * (u32)m->vtotal;
    m->refresh_mhz = total ?
        (u32)(((u64)m->pixel_clock_khz * 1000u * 1000u) / total) : 0;
    return 1;
}

/* Add a detailed timing to the mode list (deduplicated), and keep track of the
 * fastest rate seen at the native resolution - the true top of the refresh
 * choice, which the base block's single 60 Hz timing never shows. */
static u64 mode_area(const edid_mode_t *m) {
    return (u64)m->hactive * (u64)m->vactive;
}

static void update_monitor_maxima(edid_info_t *out, const edid_mode_t *m) {
    u64 area = mode_area(m);
    u64 max_area = mode_area(&out->highest_resolution);

    /* Largest raster.  Refresh is only a deterministic tie-breaker here. */
    if (!out->highest_resolution.hactive || area > max_area ||
        (area == max_area && m->hactive > out->highest_resolution.hactive) ||
        (area == max_area && m->hactive == out->highest_resolution.hactive &&
         m->refresh_mhz > out->highest_resolution.refresh_mhz))
        out->highest_resolution = *m;

    /* Fastest advertised timing at any resolution. */
    if (!out->highest_refresh.hactive ||
        m->refresh_mhz > out->highest_refresh.refresh_mhz ||
        (m->refresh_mhz == out->highest_refresh.refresh_mhz &&
         area > mode_area(&out->highest_refresh)))
        out->highest_refresh = *m;

    /* Highest valid resolution + refresh combination: maximize resolution
     * first, then choose the fastest timing actually advertised at it. */
    u64 combo_area = mode_area(&out->highest_resolution_refresh);
    if (!out->highest_resolution_refresh.hactive || area > combo_area ||
        (area == combo_area &&
         m->refresh_mhz > out->highest_resolution_refresh.refresh_mhz))
        out->highest_resolution_refresh = *m;
}

static void add_mode(edid_info_t *out, const edid_mode_t *m) {
    if (!m->hactive || !m->vactive) return;
    update_monitor_maxima(out, m);
    for (int i = 0; i < out->mode_count; i++)
        if (out->modes[i].hactive == m->hactive &&
            out->modes[i].vactive == m->vactive &&
            out->modes[i].refresh_mhz == m->refresh_mhz)
            return;                               /* already have this one     */
    if (out->mode_count < EDID_MAX_MODES)
        out->modes[out->mode_count++] = *m;
    if (out->native.hactive && m->hactive == out->native.hactive &&
        m->vactive == out->native.vactive &&
        m->refresh_mhz > out->max_refresh_mhz_at_native)
        out->max_refresh_mhz_at_native = m->refresh_mhz;
}

static void parse_hdmi_forum_vsdb(const u8 *v, u32 n, edid_info_t *out) {
    /* CTA vendor block payload starts with the LSB-first HDMI Forum OUI,
     * followed by the versioned HF-VSDB payload. */
    if (n < 7u || v[0] != 0xd8u || v[1] != 0x5du || v[2] != 0xc4u)
        return;
    const u8 *p = v + 3u; u32 pn = n - 3u;
    if (p[0] != 1u || pn < 4u) return;
    out->hdmi_forum.present = 1u;
    /* HF-VSDB Max_TMDS_Character_Rate is in 5 MHz units. */
    if (p[1]) out->hdmi_forum.max_tmds_clock_khz = (u32)p[1] * 5000u;
    out->hdmi_forum.lte_340_scramble = (p[2] >> 3) & 1u;
    out->hdmi_forum.scdc_present = (p[2] >> 7) & 1u;
    out->hdmi_forum.max_frl_rate = (p[3] >> 4) & 0xfu;
    if (out->hdmi_forum.max_frl_rate > 6u) out->hdmi_forum.max_frl_rate = 6u;
    if (pn < 10u) return;
    out->hdmi_forum.dsc_all_bpp = (p[7] >> 3) & 1u;
    out->hdmi_forum.dsc_native_420 = (p[7] >> 6) & 1u;
    out->hdmi_forum.dsc_1p2 = (p[7] >> 7) & 1u;
    u8 slice_code = p[8] & 0xfu;
    static const u8 slices[] = { 0, 1, 2, 4, 8, 8, 12, 16 };
    if (slice_code < sizeof slices)
        out->hdmi_forum.dsc_max_slices = slices[slice_code];
    out->hdmi_forum.dsc_max_pclk_per_slice_mhz = slice_code >= 5u ? 400u : 340u;
    out->hdmi_forum.dsc_max_frl_rate = (p[8] >> 4) & 0xfu;
    if (out->hdmi_forum.dsc_max_frl_rate > 6u)
        out->hdmi_forum.dsc_max_frl_rate = 6u;
    u8 chunk = p[9] & 0x3fu;
    out->hdmi_forum.dsc_total_chunk_kbytes = chunk ? (u16)chunk + 1u : 0u;
}

static void parse_hdmi_vsdb(const u8 *v, u32 n, edid_info_t *out) {
    /* HDMI Licensing OUI 0x000c03, encoded least-significant byte first.
     * In the HDMI 1.4 VSDB byte 6 after the OUI is Max_TMDS_Clock in 5 MHz
     * units.  Keep the largest value if both HDMI and HDMI Forum blocks exist. */
    if (n < 7u || v[0] != 0x03u || v[1] != 0x0cu || v[2] != 0x00u)
        return;
    u32 max_tmds = (u32)v[6] * 5000u;
    if (max_tmds > out->hdmi_forum.max_tmds_clock_khz)
        out->hdmi_forum.max_tmds_clock_khz = max_tmds;
}

int edid_parse(const u8 *block, u32 len, edid_info_t *out) {
    memset(out, 0, sizeof *out);
    if (len < EDID_BLOCK_SIZE) return 0;
    if (memcmp(block, edid_magic, 8) != 0) return 0;

    u8 sum = 0;
    for (int i = 0; i < EDID_BLOCK_SIZE; i++) sum = (u8)(sum + block[i]);
    if (sum != 0) return 0;                       /* checksum must be zero     */

    /* Manufacturer: bytes 8-9, big-endian, three 5-bit letters (1=A). */
    u16 mid = (u16)((block[8] << 8) | block[9]);
    out->manufacturer[0] = (char)(((mid >> 10) & 0x1F) + 'A' - 1);
    out->manufacturer[1] = (char)(((mid >> 5) & 0x1F) + 'A' - 1);
    out->manufacturer[2] = (char)(((mid >> 0) & 0x1F) + 'A' - 1);
    out->manufacturer[3] = 0;
    out->product_code = (u16)(block[10] | (block[11] << 8));

    /* The four 18-byte descriptors at 54, 72, 90, 108. */
    int got_native = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *d = block + 54 + i * 18;
        if (is_detailed_timing(d)) {
            edid_mode_t m;
            decode_detailed(d, &m);
            if (!got_native) { out->native = m; got_native = 1; }
            add_mode(out, &m);
        } else {
            switch (d[3]) {                        /* monitor descriptor tag   */
            case 0xFC: {                           /* the monitor's name       */
                int n = 0;
                for (int j = 5; j < 18 && n < (int)sizeof out->model - 1; j++) {
                    if (d[j] == 0x0A) break;       /* name is LF-terminated    */
                    out->model[n++] = (char)d[j];
                }
                /* trim trailing spaces */
                while (n > 0 && out->model[n - 1] == ' ') n--;
                out->model[n] = 0;
                break;
            }
            case 0xFD:                             /* the range limits         */
                out->refresh_min_hz = d[5];
                out->refresh_max_hz = d[6];
                out->hfreq_min_khz = d[7];
                out->hfreq_max_khz = d[8];
                break;
            default: break;
            }
        }
    }

    /* The base block above holds a monitor's single safe timing (usually
     * 60 Hz).  Its real high-refresh modes - 120, 144, 240 - live in the
     * CTA-861 extension blocks, whose detailed timings begin at the offset
     * named in byte 2 and run until a zero pixel clock.  Walk however many
     * blocks were handed in. */
    out->block_count = 1;
    u32 blocks = len / EDID_BLOCK_SIZE;
    for (u32 b = 1; b < blocks; b++) {
        const u8 *ext = block + b * EDID_BLOCK_SIZE;
        out->block_count++;
        u8 ext_sum = 0;
        for (u32 i = 0; i < EDID_BLOCK_SIZE; i++) ext_sum = (u8)(ext_sum + ext[i]);
        if (ext_sum != 0) continue;
        if (ext[0] == 0x02) {
            u32 dtd = ext[2];                      /* where detailed timings start */
            if (dtd < 4) continue;                 /* 0 = none; <4 is malformed */
            for (u32 off = 4u; off < dtd;) {
                u8 header = ext[off++];
                u32 tag = header >> 5, n = header & 0x1fu;
                if (off + n > dtd) break;
                if (tag == 2u) {
                    for (u32 i = 0; i < n; i++) {
                        edid_mode_t m;
                        if (edid_cea_mode(ext[off + i] & 0x7fu, &m))
                            add_mode(out, &m);
                    }
                } else if (tag == 3u) {
                    parse_hdmi_vsdb(ext + off, n, out);
                    parse_hdmi_forum_vsdb(ext + off, n, out);
                }
                off += n;
            }
            for (u32 off = dtd; off + 18 <= EDID_BLOCK_SIZE - 1; off += 18) {
                if (ext[off] == 0 && ext[off + 1] == 0) break;
                edid_mode_t m;
                decode_detailed(ext + off, &m);
                add_mode(out, &m);
            }
        } else if (ext[0] == 0x70) {
            /* DisplayID extension header is 5 bytes; byte2 is payload length.
             * Each data block is {tag, revision, payload-length}. */
            u32 end = 5u + ext[2];
            if (end > EDID_BLOCK_SIZE - 1u) end = EDID_BLOCK_SIZE - 1u;
            for (u32 off = 5u; off + 3u <= end;) {
                u8 tag = ext[off], n = ext[off + 2u];
                u32 data = off + 3u, next = data + n;
                if (next > end) break;
                if (tag == 0x03u && (n % 20u) == 0u) {
                    for (u32 pos = data; pos + 20u <= next; pos += 20u) {
                        edid_mode_t m;
                        if (decode_displayid_type1(ext + pos, &m)) add_mode(out, &m);
                    }
                } else if (tag == 0x22u) {
                    /* DisplayID 2.0 Type-VII.  Header revision bits 6:4 encode
                     * optional bytes appended to every 20-byte descriptor. */
                    u32 extra = (ext[off + 1u] >> 4) & 7u;
                    u32 stride = 20u + extra;
                    if (stride && (n % stride) == 0u) {
                        for (u32 pos = data; pos + 20u <= next; pos += stride) {
                            edid_mode_t m;
                            if (decode_displayid_type7(ext + pos, &m)) add_mode(out, &m);
                        }
                    }
                }
                off = next;
            }
        }
    }
    if (out->max_refresh_mhz_at_native == 0)
        out->max_refresh_mhz_at_native = out->native.refresh_mhz;

    out->valid = 1;
    return 1;
}
