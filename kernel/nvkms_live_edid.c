/* Repair a previously established EDID quirk on the live DP read, before
 * NVIDIA validates it. Matching all 384 bytes makes this independent of the
 * GPU port without mistaking another Acer/model/firmware for this monitor.
 * A failed read or any unrecognized payload passes through unchanged. */
#include "kernel.h"
#include "crypto.h"
#include "klog.h"
#include "nvtypes.h"

/* Exact 595.99.02 nvdp-device.h ABI; the display object remains opaque. */
extern NvBool __real_nvDPGetEDID(const void *display, void *buffer, unsigned size);

NvBool __wrap_nvDPGetEDID(const void *display, void *buffer, unsigned size) {
    NvBool ok = __real_nvDPGetEDID(display, buffer, size);
    if (!ok || !buffer || size != 384u) return ok;
    static const u8 raw_sha256[32] = {
        0x00,0x57,0x0e,0x44,0x23,0xb2,0x08,0xe0,
        0x92,0x7c,0x4a,0x22,0x62,0xa2,0x94,0x6f,
        0xf7,0x92,0x2a,0xc2,0x4c,0x4c,0xcc,0x0b,
        0x54,0x64,0xe2,0x01,0x72,0x62,0x86,0x4a
    };
    u8 digest[32];
    sha256(buffer, size, digest);
    if (memcmp(digest, raw_sha256, sizeof digest)) return ok;
    u8 *edid = buffer;
    /* Same two-byte correction as the already hardware-used saved profile:
     * 0x0d -> 0x0c removes the contradictory +255 Hz minimum flag. All mode
     * timings, identity, extension blocks and other range limits are retained. */
    edid[76] = 0x0c;
    edid[127] = (u8)(edid[127] + 1u);
    static bool reported;
    if (!reported) {
        reported = true;
        kinfo("nvkms-edid", "matched live Acer EDID capture; applied range-flag/checksum repair independent of connector");
    }
    return ok;
}
