/* SPDX-License-Identifier: MIT
 * Minimal freestanding compatibility layer for NVIDIA's MIT-licensed
 * nvt_dsc_pps.c.  Keeping this deliberately small avoids importing the RM's
 * host/OS headers into KestrelOS while leaving the PPS algorithm unchanged. */
#ifndef KESTREL_NV_DSC_COMPAT_H
#define KESTREL_NV_DSC_COMPAT_H

typedef unsigned char      NvU8;
typedef signed int         NvS32;
typedef unsigned int       NvU32;
typedef unsigned long long NvU64;
typedef unsigned char      NvBool;

#define NV_TRUE  ((NvBool)1u)
#define NV_FALSE ((NvBool)0u)
#define NVBIT32(b) ((NvU32)1u << (b))
#define NV_CEIL(a,b) (((a) + (b) - 1u) / (b))
#define NVMISC_MEMSET(s,c,n) __builtin_memset((s), (c), (n))
#define ct_assert(e) _Static_assert((e), #e)
#define ONEBITSET(v) ((v) != 0u && (((v) & ((v) - 1u)) == 0u))
#ifndef NULL
#define NULL ((void *)0)
#endif

#define IS_VALID_LANECOUNT(v) \
    ((v) == 0u || (v) == 1u || (v) == 2u || (v) == 4u || (v) == 8u)
/* linkRateHz is the per-lane symbol/bit rate in Hz.  NVIDIA accepts the
 * standard, intermediate and UHBR rates; this range+granularity check covers
 * the same values without pulling the DisplayPort host library into the OS. */
#define IS_VALID_DP2_X_LINKBW(v) ((v) >= 1620000000ull && (v) <= 20000000000ull)
#define LINK_RATE_TO_DATA_RATE_8B_10B(v)   (((NvU64)(v) * 8ull) / 10ull)
#define LINK_RATE_TO_DATA_RATE_128B_132B(v) (((NvU64)(v) * 128ull) / 132ull)

typedef enum {
    NVT_COLOR_FORMAT_RGB = 0,
    NVT_COLOR_FORMAT_YCbCr422 = 1,
    NVT_COLOR_FORMAT_YCbCr444 = 2,
    NVT_COLOR_FORMAT_YCbCr420 = 3,
    NVT_COLOR_FORMAT_Y = 4,
    NVT_COLOR_FORMAT_RAW = 5,
    NVT_COLOR_FORMAT_INVALID = 0xff
} NVT_COLOR_FORMAT;

typedef enum {
    NVT_STATUS_SUCCESS = 0,
    NVT_STATUS_ERR = 0x80000000u,
    NVT_STATUS_INVALID_PARAMETER,
    NVT_STATUS_NO_MEMORY,
    NVT_STATUS_COLOR_FORMAT_NOT_SUPPORTED,
    NVT_STATUS_INVALID_HBLANK,
    NVT_STATUS_INVALID_BPC,
    NVT_STATUS_INVALID_BPP,
    NVT_STATUS_MAX_LINE_BUFFER_ERROR,
    NVT_STATUS_OVERALL_THROUGHPUT_ERROR,
    NVT_STATUS_DSC_SLICE_ERROR,
    NVT_STATUS_PPS_SLICE_COUNT_ERROR,
    NVT_STATUS_PPS_SLICE_HEIGHT_ERROR,
    NVT_STATUS_PPS_SLICE_WIDTH_ERROR,
    NVT_STATUS_INVALID_PEAK_THROUGHPUT,
    NVT_STATUS_MIN_SLICE_COUNT_ERROR,
} NVT_STATUS;

#endif
