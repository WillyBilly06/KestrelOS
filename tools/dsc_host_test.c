#include <stdio.h>
#include <string.h>
#include "nv_dsc_pps.h"

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

int main(void) {
    DSC_INFO d; MODESET_INFO m; WAR_DATA w;
    unsigned pps[DSC_MAX_PPS_SIZE_DWORD], bpp = 0, mask = 0;
    memset(&d, 0, sizeof d); memset(&m, 0, sizeof m); memset(&w, 0, sizeof w);
    d.sinkCaps.decoderColorFormatMask = DSC_DECODER_COLOR_FORMAT_RGB;
    d.sinkCaps.bitsPerPixelPrecision = DSC_BITS_PER_PIXEL_PRECISION_1_16;
    d.sinkCaps.maxSliceWidth = 2560; d.sinkCaps.maxNumHztSlices = 8;
    d.sinkCaps.sliceCountSupportedMask = DSC_DECODER_SLICES_PER_SINK_1 |
        DSC_DECODER_SLICES_PER_SINK_2 | DSC_DECODER_SLICES_PER_SINK_4 |
        DSC_DECODER_SLICES_PER_SINK_8;
    d.sinkCaps.lineBufferBitDepth = 13;
    d.sinkCaps.decoderColorDepthCaps = DSC_DECODER_COLOR_DEPTH_CAPS_8_BITS;
    d.sinkCaps.decoderColorDepthMask = DSC_DECODER_COLOR_DEPTH_CAPS_8_BITS;
    d.sinkCaps.algorithmRevision.versionMajor = 1;
    d.sinkCaps.algorithmRevision.versionMinor = 2;
    d.sinkCaps.bBlockPrediction = NV_TRUE;
    d.sinkCaps.peakThroughputMode0 = DSC_DECODER_PEAK_THROUGHPUT_MODE0_340;
    d.sinkCaps.maxBitsPerPixelX16 = 24 * 16;
    d.gpuCaps.encoderColorFormatMask = DSC_ENCODER_COLOR_FORMAT_RGB;
    d.gpuCaps.lineBufferSize = 5; d.gpuCaps.bitsPerPixelPrecision = 1;
    d.gpuCaps.maxNumHztSlices = 8; d.gpuCaps.lineBufferBitDepth = 13;
    m.pixelClockHz = 1050000000ull; m.activeWidth = 2560; m.activeHeight = 1440;
    m.bitsPerComponent = 8; m.colorFormat = NVT_COLOR_FORMAT_RGB;
    w.connectorType = DSC_DP; w.dpData.linkRateHz = 8100000000ull;
    w.dpData.laneCount = 4; w.dpData.dpMode = DSC_DP_SST; w.dpData.hBlank = 160;
    NVT_STATUS st = DSC_GeneratePPSWithSliceCountMask(&d, &m, &w,
        8100000000ull * 4ull * 8ull / 10ull * 97ull / 100ull,
        pps, &bpp, &mask);
    const unsigned char *p = (const unsigned char *)pps;
    unsigned sw = be16(p + 12), sh = be16(p + 10);
    int fail = 0;
    if (st != NVT_STATUS_SUCCESS || p[0] != 0x12 || be16(p + 6) != 1440 ||
        be16(p + 8) != 2560 || !sw || !sh || 2560 % sw ||
        bpp < 8 * 16 || bpp > 24 * 16 || !mask) fail++;
    printf("DSC PPS: status=%#x bpp=%u/16 slice=%ux%u mask=%#x\n",
           (unsigned)st, bpp, sw, sh, mask);
    printf(fail ? "FAIL: %d failures\n" : "ALL GOOD: NVIDIA DSC PPS generator\n", fail);
    return fail != 0;
}
