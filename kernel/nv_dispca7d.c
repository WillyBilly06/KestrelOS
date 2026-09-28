/* nv_dispca7d.c - DisplayPort re-light on Blackwell GB202 through GSP-RM.
 *
 * Booting the GSP takes the panel off the firmware framebuffer, so after GSP
 * bring-up the monitor is frozen.  This file lights ONE DisplayPort output back
 * up at a fixed EDID mode and points its scanout window at the boot framebuffer
 * (g_boot.fb.base) - the same memory the desktop already composites into - so an
 * accelerated desktop becomes visible with no extra copy.
 *
 * It is the GSP-era display path (not nv_disp.c's pre-Turing PDISP, nor the
 * wrong-generation model in nv_dispc37d.c).  Every constant here was diffed
 * against the reference driver, not eyeballed:
 *   - RM controls + param structs: r535/nvrm/disp.h
 *   - disp object/channel alloc order: r535/disp.c (r535_disp_chan_set_pushbuf,
 *     r535_dmac_alloc) - pushbuffer address registered BEFORE the channel alloc
 *   - core/window method offsets + fields: include/nvhw/class/clca7d.h, clca7e.h
 *   - flat-DMA push-header encoding: clc97b.h  (NOT GPFIFO)
 *   - PUT kick register: NV507C_PUT=0x0 within the channel USER block, core at
 *     BAR0 0x680000, window at 0x690000 + head*0x1000 (r535_chan_user)
 *
 * SAFETY: UPDATE on an untrained link can black the panel with no error, so the
 * whole light-up is gated behind the "displaytest" cmdline flag by the caller,
 * and SOR_SET_CONTROL+UPDATE are only pushed when DP link training returned
 * success (not -EAGAIN / pending).  Every step logs its RM return code so one
 * boot is diagnostic.  This is build-verified and source-cross-checked; it has
 * NOT run on silicon.
 *
 * ---- 2026-09 object-tree rewrite (match r535_disp_oneinit + r535_disp_init) ----
 * The re-light died at the very first object alloc because we never actually
 * allocated the NV04_DISPLAY_COMMON "objcom" object - we just fired NV0073
 * controls at the bare handle 0x00730000 with no object there, and we allocated
 * the disp objects under the SHARED RM device instead of a dedicated disp
 * client+device.  nouveau's r535_disp_oneinit (disp.c:1503) does, in order:
 *   RAMIN (nvkm_gpuobj_new, disp.c:1513) -> WRITE_INST_MEM on the internal
 *   privileged subdevice (disp.c:1520-1531) -> a DEDICATED disp client+device
 *   (nvkm_gsp_client_device_ctor, disp.c:1536) -> allocate NV04_DISPLAY_COMMON
 *   objcom under THAT disp device (handle NVKM_RM_DISP=0x00730000, no params,
 *   disp.c:1540) -> get_static_info on the internal subdevice (disp.c:1545) ->
 *   DP_SET_MANUAL_DISPLAYPORT once on the objcom (disp.c:1620-1633).  Then
 *   r535_disp_init (disp.c:1455-1466) allocs the 0xca70 disp-engine root under
 *   the SAME disp device.  Every NV0073 control (GET_SUPPORTED, OR_GET_INFO,
 *   DP_GET_CAPS, DFP_ASSIGN_SOR, DP_CTRL, DP_CONFIG_STREAM) targets the objcom
 *   handle (disp->rm.objcom), NOT the shared device.
 *
 * Command ids ADDED/verified this pass against the OGKM ctrl0073 headers
 * (local open-gpu-kernel-modules checkout\src\common\sdk\nvidia\inc\ctrl\ctrl0073):
 *   ADDED  NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT = 0x731365
 *          (ctrl0073dp.h:1596; params = {u32 subDeviceInstance}, 4 bytes,
 *           gsp_abi_check.c:639).  We had NO define for it before.
 *   VERIFIED unchanged (all already correct):
 *     SYSTEM_GET_SUPPORTED 0x730107 (ctrl0073system.h:310),
 *     SPECIFIC_OR_GET_INFO 0x73028b (ctrl0073specific.h:1076),
 *     DP_GET_CAPS 0x731369 (ctrl0073dp.h:1718),
 *     DFP_ASSIGN_SOR 0x731152 (ctrl0073dfp.h:598),
 *     DP_CTRL 0x731343 (ctrl0073dp.h:441),
 *     DP_CONFIG_STREAM 0x731362 (ctrl0073dp.h:1479).
 *   NV04_DISPLAY_COMMON = 0x0073 (r535/nvrm/disp.h:21); objcom handle
 *     NVKM_RM_DISP = 0x00730000 (nouveau rm/handles.h:14).
 *
 * New prototype relied on (added by a concurrent agent to nv_gsp_rm.c; the main
 * agent will move the canonical decl to nv.h), forward-declared below:
 *     bool nv_rm_disp_client_ctor(nv_card_t *, nv_rm_t *, u32 *client,
 *                                 u32 *device, u32 *subdevice);
 *
 * TODOs left because the source value could not be pinned here:
 *   - RAMIN parent: nouveau's RAMIN is nvkm_gpuobj_new on the base device
 *     (disp.c:1513), which is target-VRAM; we keep nv_vram_alloc under
 *     H_DISP_RAMIN.  If a finding shows the instance block must live under the
 *     dedicated disp client, only the nv_vram_alloc parent needs to change - the
 *     WRITE_INST_MEM still registers the same physical FB address.
 */
#include "kernel.h"
#include "mm.h"
#include "klog.h"
#include "nv.h"
#include "edid.h"
#include "nv_dsc_pps.h"
#include "time.h"

/* Build the DEDICATED disp client+device+subdevice that nouveau's
 * r535_disp_oneinit uses (nvkm_gsp_client_device_ctor, disp.c:1536) - the disp
 * objcom (NV04_DISPLAY_COMMON) and the 0xca70 disp-engine root hang off THIS
 * device, not the shared RM_DEVICE.  Provided by a concurrent agent in
 * nv_gsp_rm.c; the main agent will move the canonical decl to nv.h.  Returns
 * the three handles; false on failure. */
extern bool nv_rm_disp_client_ctor(nv_card_t *c, nv_rm_t *rm,
                                    u32 *out_client, u32 *out_device,
                                    u32 *out_subdevice);

/* RM object handles (mirror nv_gsp_rm.c's, which are file-static there). */
#define RM_DEVICE     0xDE1D0000u
#define RM_SUBDEVICE  0x5D1D0000u
#define RM_DISP       0x00730000u

/* ------------------------------------------------------------- RM classes */
#define GB202_DISP                  0x0000ca70u
#define GB202_DISP_CORE_CHANNEL_DMA 0x0000ca7du
#define GB202_DISP_WINDOW_CHANNEL_DMA 0x0000ca7eu

/* handles: nouveau uses (oclass<<16) for the disp root and (oclass<<16)|inst
 * for each channel (r535/disp.c). */
#define H_DISP_ROOT   (GB202_DISP << 16)                 /* 0xca700000 */
#define H_CORE_CHAN   ((GB202_DISP_CORE_CHANNEL_DMA << 16) | 0u)
#define H_WINDOW_CHAN ((GB202_DISP_WINDOW_CHANNEL_DMA << 16) | 0u)
#define H_DISP_RAMIN  0x00d10000u
#define H_DISP_SCANOUT 0x00d20000u   /* dedicated solid-colour scanout surface (RGB test) */
#define H_DISP_SCANOUT_ALT 0x00d20001u /* live-EDID-sized replacement surface */
#define H_DISP_RMCTRL  0xc3720000u   /* NVC372_DISPLAY_SW, NVKMS rmCtrlHandle */

#define NVC372_DISPLAY_SW                         0x0000c372u
#define NVC372_CTRL_CMD_IS_MODE_POSSIBLE          0xc3720101u
#define NVC372_CTRL_IMP_MAX_HEADS                 8u
#define NVC372_CTRL_IMP_MAX_WINDOWS               32u
#define NVC372_CTRL_IMP_MAX_TILES                 8u
#define NVC372_CTRL_IMP_NEED_MIN_VPSTATE          0x00000002u
#define NVC372_CTRL_IMP_LUT_1025                  2u
#define NVC372_CTRL_IMP_FORMAT_RGB_PACKED_4_BPP   0x00000004u

/* --------------------------------------------------- RM control command ids
 * (r535/nvrm/disp.h - verified byte-for-byte). */
#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM   0x20800a49u
#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO  0x20800a01u
#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER 0x20800a58u
#define NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED   0x730107u   /* verified vs OGKM 595 ctrl0073system.h:310 + r570; 0x730120 is EXECUTE_ACPI_METHOD (the old value here aborted the whole re-light at the first query) */
#define NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE      0x73010cu   /* 595 ctrl0073system.h:646; r535's 0x730126 is stale */
#define NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO   0x73028bu
#define NV0073_CTRL_CMD_DP_GET_CAPS            0x731369u
#define NV0073_CTRL_CMD_DFP_ASSIGN_SOR         0x731152u
#define NV0073_CTRL_CMD_DP_CTRL                0x731343u
#define NV0073_CTRL_CMD_DP_CONFIG_STREAM       0x731362u
#define NV0073_CTRL_CMD_DP_SET_MSA_PROPERTIES  0x73136au
#define NV0073_CTRL_CMD_DP_SET_STEREO_MSA_PROPERTIES 0x731378u
#define NV0073_CTRL_CMD_DP_CONFIGURE_FEC       0x73137au
#define NV0073_CTRL_CMD_CALCULATE_DP_IMP        0x73138cu
#define NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_ENABLE 0x730273u
#define NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_SINK_CAPS 0x730293u
#define NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_FRL_CONFIG 0x73029au
#define NV0073_CTRL_CMD_SPECIFIC_GET_HDMI_GPU_CAPS 0x7302a2u
#define NV0073_CTRL_CMD_SPECIFIC_GET_HDMI_FRL_CAPACITY_COMPUTATION 0x7302a8u
/* DP_SET_MANUAL_DISPLAYPORT: issued once in oneinit to disable GSP-RM's
 * automatic watermark/CP-IRQ/defer handling so the manual modeset below takes
 * effect (OGKM ctrl0073dp.h:1596; params are just {u32 subDeviceInstance},
 * size 4 - gsp_abi_check.c:639).  We had NO define for this before. */
#define NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT 0x731365u
/* SPECIFIC_DISPLAY_CHANGE - the modeset BRACKET (ctrl0073specific.h:1526-1541,
 * used by nvkms-rm.c:1946 nvRmBeginEndModeset).  START before pushing the head/SOR
 * modeset, END after - this is what tells GSP-RM a display change is in progress
 * so it FINALIZES the SOR pad enable.  Missing it = raster runs but the SOR never
 * transmits (monitor "No Input" despite advancing RG LOADV). */
#define NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE 0x7302a4u
#define NV0073_DISPLAY_CHANGE_END   0u
#define NV0073_DISPLAY_CHANGE_START 1u
typedef struct {
    u32 subDeviceInstance;
    u32 newDevices;     /* the NEW display config mask (the displayId we drive) */
    u32 properties;
    u32 enable;         /* START(1) before / END(0) after */
} sys_display_change_t;

/* Disp objcom class + handle (r535/nvrm/disp.h:21, nouveau rm/handles.h:14).
 * The NV04_DISPLAY_COMMON object is the parent of ALL NV0073 controls; nouveau
 * allocates it under a dedicated disp client+device (disp.c:1540) with the fixed
 * handle NVKM_RM_DISP.  Our RM_DISP already equals NVKM_RM_DISP = 0x00730000. */
#define NV04_DISPLAY_COMMON   0x00000073u
#define NVKM_RM_DISP          RM_DISP            /* 0x00730000, the objcom handle */

#define DISP_ADDR_SYSMEM  1u
#define DISP_ADDR_FBMEM   2u
#define DISP_MEM_WRITECOMBINED 2u
#define DISP_CACHE_SNOOP_COHERENT 1u

/* SOR protocol + OR type (disp.h). */
#define OR_TYPE_SOR            2u
#define OR_PROTOCOL_SOR_DP_A   8u
#define OR_PROTOCOL_SOR_DP_B   9u
#define OR_PROTOCOL_SOR_TMDS_A 1u
#define OR_PROTOCOL_SOR_TMDS_B 2u
#define OR_PROTOCOL_SOR_TMDS_DUAL 5u
#define OR_PROTOCOL_SOR_HDMI_FRL 12u

/* DP_CTRL cmd/data bitfields - each VERIFIED against OGKM
 * local open-gpu-kernel-modules checkout\src\common\sdk\nvidia\inc\ctrl\ctrl0073\ctrl0073dp.h
 * (line cites inline).  The three that were already here (SET_LANE_COUNT,
 * SET_LINK_BW, SET_ENHANCED_FRAMING cmd bits and the LANE_COUNT/LINK_BW data
 * shifts) all matched the header exactly; this pass ADDS TRAIN_PHY_REPEATER and
 * the TARGET shift, and DELETES the two data-side defines that diverged from
 * nouveau (see r535_dp_train_target, disp.c:971-973: data carries only
 * SET_LANE_COUNT | SET_LINK_BW | TARGET - NOT enhanced-framing). */
#define DP_CMD_SET_LANE_COUNT       (1u << 0)   /* SET_LANE_COUNT 0:0, _TRUE=1  (ctrl0073dp.h:455,457) */
#define DP_CMD_SET_LINK_BW          (1u << 1)   /* SET_LINK_BW    1:1, _TRUE=1  (ctrl0073dp.h:458,460) */
#define DP_CMD_SET_ENHANCED_FRAMING (1u << 7)   /* SET_ENHANCED_FRAMING 7:7, _TRUE=1 (ctrl0073dp.h:474,476) */
#define DP_CMD_TRAIN_PHY_REPEATER   (1u << 13)  /* TRAIN_PHY_REPEATER 13:13, _YES=1 (ctrl0073dp.h:490,492) */
#define DP_CMD_ENABLE_FEC           (1u << 15)  /* ENABLE_FEC 15:15: sink before LT; GPU after LT */
#define DP_DATA_LANE_COUNT_SHIFT    0           /* SET_LANE_COUNT 4:0   (ctrl0073dp.h:510) */
#define DP_DATA_LINK_BW_SHIFT       8           /* SET_LINK_BW   15:8   (ctrl0073dp.h:516) */
#define DP_DATA_TARGET_SHIFT        19          /* TARGET        22:19  (ctrl0073dp.h:528); _SINK=0 (ctrl0073dp.h:529) */
/* card DP_GET_CAPS maxLinkRate code (MAX_LINK_RATE 2:0, ctrl0073dp.h:1754):
 *   _NONE=0 _1_62=1 _2_70=2 _5_40=3 _8_10=4 (ctrl0073dp.h:1755-1759)
 * mapped to the SET_LINK_BW data code below (ctrl0073dp.h:517,520,523,524),
 * which is ALSO the encoding the sink DPCD 0x1 uses - so card and sink ceilings
 * are directly comparable once the card code is mapped through here. */
#define DP_LINK_BW_1_62 0x06u                   /* ctrl0073dp.h:517 (_1_62GBPS) */
#define DP_LINK_BW_2_70 0x0au                   /* ctrl0073dp.h:520 (_2_70GBPS) */
#define DP_LINK_BW_5_40 0x14u                   /* ctrl0073dp.h:523 (_5_40GBPS) */
#define DP_LINK_BW_8_10 0x1eu                   /* ctrl0073dp.h:524 (_8_10GBPS) */

/* ------------------------------------------------------- RM param structs
 * Faithful to r535/nvrm/disp.h.  Non-packed with natural u64 alignment so the
 * compiler inserts the SAME padding NV_DECLARE_ALIGNED produces (FINN validates
 * the exact struct size, so the layout must match). */
typedef struct {
    u64 instMemPhysAddr;
    u64 instMemSize;
    u32 instMemAddrSpace;
    u32 instMemCpuCacheAttr;
} disp_write_inst_mem_t;                         /* 24 bytes */

/* NV2080_CTRL_INTERNAL_DISPLAY_GET_STATIC_INFO_PARAMS - EXACT layout
 * (ctrl2080internal.h:72-83).  The old struct had a bogus bPrimaryVga and was
 * missing embeddedDisplayPortMask, bExternalMuxSupported, bInternalMuxSupported
 * and numDispChannels, so it was the wrong size and GSP-RM refused the control
 * (proven: "static info refused, windows 0 heads 0"). */
typedef struct {
    u32 feHwSysCap;
    u32 windowPresentMask;
    u8  bFbRemapperEnabled;          /* NvBool */
    u32 numHeads;
    u32 i2cPort;
    u32 internalDispActiveMask;
    u32 embeddedDisplayPortMask;     /* WAS MISSING */
    u8  bExternalMuxSupported;       /* NvBool, WAS MISSING */
    u8  bInternalMuxSupported;       /* NvBool, WAS MISSING */
    u32 numDispChannels;             /* WAS MISSING */
} disp_get_static_info_t;

/* NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS - EXACT layout
 * (ctrl2080internal.h:1367-1378).  The old struct was missing pbTargetAperture,
 * channelPBSize and subDeviceId -> wrong size -> the CHANNEL_PUSHBUFFER control
 * was refused, which is what failed the core-channel alloc.  nouveau leaves the
 * three trailing fields zeroed (r535 disp.c:84-108), so a memset is correct. */
typedef struct {
    u32 addressSpace;
    u64 physicalAddr;                /* NV_DECLARE_ALIGNED(,8) - natural u64 align */
    u64 limit;
    u32 cacheSnoop;
    u32 hclass;
    u32 channelInstance;
    u8  valid;                       /* NvBool */
    u32 pbTargetAperture;            /* WAS MISSING */
    u32 channelPBSize;               /* WAS MISSING */
    u32 subDeviceId;                 /* WAS MISSING */
} disp_channel_pushbuffer_t;                     /* 56 bytes */

/* NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS - EXACT layout (nvos.h:2484-2501).
 * Was missing channelPBSize and subDeviceId (32 vs 40 bytes) - the disp-channel
 * object alloc would be refused for the same size reason.  nouveau's
 * r535_dmac_alloc (disp.c:178-179) sets only channelInstance + offset and leaves
 * the rest zeroed, so a memset matches. */
typedef struct {
    u32 channelInstance;
    u32 hObjectBuffer;
    u32 hObjectNotify;
    u32 offset;
    u64 pControl;                    /* NV_ALIGN_BYTES(8) */
    u32 flags;
    u32 channelPBSize;               /* ChannelPBSize enum, WAS MISSING */
    u32 subDeviceId;                 /* WAS MISSING */
} vaio_channeldma_alloc_t;                       /* 40 bytes */

typedef struct {
    u32 subDeviceInstance;
    u32 displayMask;
    u32 displayMaskDDC;
} sys_get_supported_t;

/* NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE (0x730108, ctrl0073system.h:392-401):
 * pass the supported mask in displayMask + flags 0; it returns the CONNECTED
 * subset in displayMask.  GET_SUPPORTED reports every POSSIBLE display slot
 * (0x7f00 here) - most are empty.  AUXCH_CTRL and DP_CTRL talk to the physical
 * sink, so aimed at an UNCONNECTED displayId they return INVALID_ARGUMENT (0x1f),
 * exactly what the last boot showed while the static OR_GET_INFO/DFP_ASSIGN_SOR
 * on the same id succeeded. */
#define NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE 0x730108u
#define NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2      0x730245u
#define NV0073_GET_EDID_MAX_BYTES                 2048u
/* flags METHOD field 1:0 (ctrl0073system.h:404-407): _CACHED = 1.  The driver's
 * kdispIsDisplayConnected (kern_disp.c:1629-1631) uses METHOD_CACHED - the GSP's
 * boot-time detection result - not a fresh probe.  Match it. */
#define NV0073_CONNECT_STATE_FLAGS_METHOD_CACHED 0x1u
typedef struct {
    u32 subDeviceInstance;
    u32 flags;
    u32 displayMask;     /* in: mask to test; out: connected subset */
    u32 retryTimeMs;
} sys_get_connect_state_t;

/* NV0073_CTRL_SPECIFIC_GET_EDID_V2_PARAMS, byte-for-byte with NVIDIA 595
 * ctrl0073specific.h and gsp_abi_check.c (2064 bytes).  One display-id per
 * request is mandatory.  COPY_CACHE first consumes RM's hotplug result without
 * disturbing a live link; RAW/DDC is the fallback for a newly attached sink. */
typedef struct {
    u32 subDeviceInstance;
    u32 displayId;
    u32 bufferSize;
    u32 flags;
    u8  edidBuffer[NV0073_GET_EDID_MAX_BYTES];
} get_edid_v2_t;
_Static_assert(sizeof(get_edid_v2_t) == 2064, "GET_EDID_V2 ABI is 2064 bytes");
#define GET_EDID_COPY_CACHE_YES  (1u << 0)
#define GET_EDID_READ_MODE_RAW   (1u << 1)

#define NV_DISP_MAX_SINKS 4u
typedef struct {
    bool valid;
    bool active_at_boot;
    bool attach_attempted;
    u32 display_id;
    u32 boot_head;
    u32 assigned_head;
    u32 assigned_sor;
    u32 edid_len;
    edid_info_t edid;
    edid_mode_t best_mode;
} disp_sink_t;

/* GET_EDID_V2 is 2064 bytes.  Keep both the RPC buffer and parsed sink table
 * out of the small kernel stack.  They are rebuilt on every display light-up. */
static get_edid_v2_t g_edid_rpc;
static disp_sink_t g_disp_sinks[NV_DISP_MAX_SINKS];
static u32 g_disp_sink_count;
static edid_info_t g_boot_edid;

/* Current 595 ABI.  This reports the display class's active output for a head,
 * independently of our raw ASSY/ARM register decoding. */
typedef struct {
    u32 subDeviceInstance;
    u32 head;
    u32 flags;
    u32 displayId;
} sys_get_active_t;                              /* 16 bytes */

/* DP_SET_MANUAL_DISPLAYPORT params - just the subdevice instance
 * (OGKM ctrl0073dp.h:1600-1602; FINN size 4, gsp_abi_check.c:639). */
typedef struct {
    u32 subDeviceInstance;
} dp_set_manual_displayport_t;                   /* 4 bytes */

typedef struct {
    u32 subDeviceInstance;
    u32 displayId;
    u32 index;
    u32 type;
    u32 protocol;
    u32 ditherType;
    u32 ditherAlgo;
    u32 location;
    u32 rootPortId;
    u32 dcbIndex;
    u64 vbiosAddress;
    u8  bIsLitByVbios;
    u8  bIsDispDynamic;
} or_get_info_t;

typedef struct {
    u32 bDscSupported;   /* NvBool, but padded; keep u32 for the layout below */
    u32 encoderColorFormatMask;
    u32 lineBufferSizeKB;
    u32 rateBufferSizeKB;
    u32 bitsPerPixelPrecision;
    u32 maxNumHztSlices;
    u32 lineBufferBitDepth;
} dp_dsc_cap_t;

/* NV0073_CTRL_CMD_DP_GET_CAPS_PARAMS - EXACT layout (ctrl0073dp.h:1722-1740).
 * The old struct was missing minPClkForCompressed, bUseRgFlushSequence and
 * bSupportDPDownSpread, so its size was wrong and GSP-RM rejected the control
 * with NV_ERR_INVALID_ARGUMENT(31) - proven on silicon (command 00731369
 * refused (31)).  NvBool == u8; natural alignment matches NVIDIA's build. */
typedef struct {
    u32 subDeviceInstance;
    u32 sorIndex;
    u32 maxLinkRate;
    u32 dpVersionsSupported;
    u32 UHBRSupported;               /* UHBRSupportedByGpu (ctrl0073dp.h:1727) */
    u32 minPClkForCompressed;        /* ctrl0073dp.h:1728 - WAS MISSING          */
    u8  bIsMultistreamSupported;
    u8  bIsSCEnabled;
    u8  bHasIncreasedWatermarkLimits;
    u8  bIsPC2Disabled;
    u8  isSingleHeadMSTSupported;
    u8  bFECSupported;
    u8  bIsTrainPhyRepeater;
    u8  bOverrideLinkBw;
    u8  bUseRgFlushSequence;         /* ctrl0073dp.h:1737 - WAS MISSING           */
    u8  bSupportDPDownSpread;        /* ctrl0073dp.h:1738 - WAS MISSING           */
    dp_dsc_cap_t DSC;                /* NV0073_CTRL_CMD_DSC_CAP_PARAMS, ctrl0073common.h:62 */
} dp_get_caps_t;

typedef struct { u32 displayMask; u32 sorType; } dfp_sor_info_t;
typedef struct {
    u32 subDeviceInstance;
    u32 displayId;
    u8  sorExcludeMask;
    u32 slaveDisplayId;
    u32 forceSublinkConfig;
    u8  bIs2Head1Or;
    u32 sorAssignList[4];
    dfp_sor_info_t sorAssignListWithTag[4];
    u8  reservedSorMask;
    u32 flags;
} dfp_assign_sor_t;

typedef struct {
    u32 subDeviceInstance;
    u32 displayId;
    u32 cmd;
    u32 data;
    u32 err;
    u32 retryTimeMs;
    u32 eightLaneDpcdBaseAddr;
} dp_ctrl_t;

typedef struct {
    u32 subDeviceInstance;
    u32 displayId;
    u8 bEnableFec;
} dp_configure_fec_t;
_Static_assert(sizeof(dp_configure_fec_t) == 12, "DP_CONFIGURE_FEC ABI");

/* Exact ctrl0073dp.h MSA ABI.  For normal RGB/non-VRR NVKMS deliberately
 * sends zero feature masks with bEnableMSA=0: this clears any stale override,
 * while bCacheMsaOverrideForNextModeset makes RM apply that state to the core
 * update which immediately follows.  A cold DP head does not have the UEFI
 * primary's inherited MSA state, so omitting these pre-modeset calls leaves a
 * trained receiver but an ownerless SOR (the returned 09:47 result). */
typedef struct {
    u8 miscMask[2];
    u8 bRasterTotalHorizontal, bRasterTotalVertical;
    u8 bActiveStartHorizontal, bActiveStartVertical;
    u8 bSurfaceTotalHorizontal, bSurfaceTotalVertical;
    u8 bSyncWidthHorizontal, bSyncPolarityHorizontal;
    u8 bSyncHeightVertical, bSyncPolarityVertical;
    u8 bReservedEnable[3];
} dp_msa_mask_t;
typedef struct {
    u8 misc[2];
    u16 rasterTotalHorizontal, rasterTotalVertical;
    u16 activeStartHorizontal, activeStartVertical;
    u16 surfaceTotalHorizontal, surfaceTotalVertical;
    u16 syncWidthHorizontal, syncPolarityHorizontal;
    u16 syncHeightVertical, syncPolarityVertical;
    u8 reserved[3];
} dp_msa_values_t;
typedef struct {
    u32 subDeviceInstance, displayId;
    u8 bEnableMSA, bStereoPhaseInverse, bCacheMsaOverrideForNextModeset;
    dp_msa_mask_t featureMask;
    dp_msa_values_t featureValues;
    u64 pFeatureDebugValues;
} dp_set_msa_t;
typedef struct {
    u32 subDeviceInstance, displayId;
    u8 bEnableMSA, bStereoPhaseInverse;
    dp_msa_mask_t featureMask;
    dp_msa_values_t featureValues;
} dp_set_stereo_msa_t;
_Static_assert(sizeof(dp_msa_mask_t) == 15, "DP MSA mask ABI");
_Static_assert(sizeof(dp_msa_values_t) == 26, "DP MSA values ABI");
_Static_assert(sizeof(dp_set_msa_t) == 64, "DP SET_MSA_PROPERTIES ABI");
_Static_assert(sizeof(dp_set_stereo_msa_t) == 52, "DP SET_STEREO_MSA ABI");

typedef struct {
    u32 linkRate10M, laneCount;
    u8 bEnhancedFraming, bDp2xChannelCoding, bMultiStreamTopology, bFECEnabled;
} dp_imp_link_t;
typedef struct {
    u32 rasterWidth, rasterHeight, surfaceWidth, surfaceHeight;
    u32 rasterBlankStartX, rasterBlankEndX, depth;
    u32 twoChannelAudioHz, eightChannelAudioHz, pixelFrequencyKHz;
    u32 bitsPerComponent, colorFormat;
    u8 bDSCEnabled;
} dp_imp_mode_t;
typedef struct { u32 sliceCount, sliceWidth, sliceHeight, dscVersionMajor, dscVersionMinor; } dp_imp_dsc_t;
typedef struct { u32 waterMark, tuSize, minHBlank, hBlankSym, vBlankSym, effectiveBpp; u8 bIsModePossible; } dp_imp_wm_t;
typedef struct {
    u32 subDeviceInstance, displayId, headIndex;
    dp_imp_link_t linkConfig;
    dp_imp_mode_t modesetInfo;
    dp_imp_dsc_t dscInfo;
    dp_imp_wm_t watermark;
} dp_imp_t;
_Static_assert(sizeof(dp_imp_t) == 124, "CALCULATE_DP_IMP ABI");

typedef struct {
    bool valid, fec_capable;
    u32 revision_major, revision_minor;
    u32 rc_buffer_kb, slice_mask, max_slices, max_slice_width;
    u32 line_buffer_depth, bpp_precision, max_bpp_x16;
    u32 color_formats, color_depths, peak_throughput0, peak_throughput1;
    bool block_prediction;
} dp_sink_dsc_t;

typedef struct {
    bool enabled;
    u32 bits_per_pixel_x16, slice_count, slice_width, slice_height;
    u32 pps[DSC_MAX_PPS_SIZE_DWORD];
    u32 wm, tu, hblank_sym, vblank_sym;
} dp_mode_plan_t;

typedef struct { u32 subDeviceInstance, displayId, caps; } hdmi_sink_caps_rm_t;
typedef struct { u32 subDeviceInstance, caps; } hdmi_gpu_caps_rm_t;
typedef struct { u8 subDeviceInstance; u32 displayId; u8 enable; } hdmi_enable_rm_t;
typedef struct {
    u32 numLanes, frlBitRateGbps, pclk10KHz, hTotal, hActive, bpc;
    u32 pixelPacking, audioType, numAudioChannels, audioFreqKHz;
    struct { u32 bppTargetx16, hSlices, sliceWidth, dscTotalChunkKBytes; } compressionInfo;
} hdmi_frl_input_t;
typedef struct {
    u32 frlRate, bppTargetx16;
    u8 engageCompression, isAudioSupported, dataFlowDisparityReqMet;
    u8 dataFlowMeteringReqMet, isVideoTransportSupported;
    u32 triBytesBorrowed, hcActiveBytes, hcActiveTriBytes, hcBlankTriBytes;
    u32 tBlankToTTotalX1k;
} hdmi_frl_result_t;
typedef struct { u32 vic, packing, bpc, frlRate, bppX16; u8 bHasPreCalcFRLData; } hdmi_frl_precalc_t;
typedef struct { u32 maxSliceCount, maxSliceWidth; u8 bIsDSCPossible; } hdmi_frl_dsc_possible_t;
typedef struct {
    u8 cmd;
    hdmi_frl_input_t input;
    hdmi_frl_result_t result;
    hdmi_frl_precalc_t preCalc;
    hdmi_frl_dsc_possible_t dsc;
} hdmi_frl_capacity_t;
typedef struct { u32 subDeviceInstance, displayId, data; u8 bFakeLt, bLtSkipped; } hdmi_frl_config_t;
_Static_assert(sizeof(hdmi_frl_capacity_t) == 132, "HDMI FRL capacity ABI");
_Static_assert(sizeof(hdmi_frl_config_t) == 16, "HDMI FRL config ABI");
_Static_assert(sizeof(hdmi_enable_rm_t) == 12, "HDMI enable ABI");

typedef struct {
    bool dsc;
    u32 frl_rate, bpp_x16, slices, slice_width;
    u32 hactive_bytes, hactive_tribytes, hblank_tribytes;
    u32 pps[DSC_MAX_PPS_SIZE_DWORD];
} hdmi_mode_plan_t;

/* NV0073_CTRL_CMD_DP_CONFIG_STREAM_PARAMS - EXACT layout (ctrl0073dp.h:1483-1513).
 * The OLD struct here carried an SST.Legacy/MvidWarParams tail that does NOT
 * exist in this GSP's ABI (that is an older/different driver variant), so its
 * size was wrong and GSP-RM would refuse DP_CONFIG_STREAM - leaving the DP SST
 * watermarks unconfigured, so nothing scans out even after the modeset.  This is
 * the exact 84-byte layout for 595/570.  With bEnableOverride=0 RM computes
 * hBlankSym/vBlankSym/waterMark itself. */
typedef struct {
    u32 subDeviceInstance;
    u32 head;
    u32 sorIndex;
    u32 dpLink;
    u8  bEnableOverride;             /* NvBool */
    u8  bMST;                        /* NvBool */
    u32 singleHeadMultistreamMode;
    u32 hBlankSym;
    u32 vBlankSym;
    u32 colorFormat;
    u8  bEnableTwoHeadOneOr;         /* NvBool */
    struct {
        u32 slotStart;
        u32 slotEnd;
        u32 PBN;
        u32 Timeslice;
        u8  sendACT;                 /* NvBool */
        u32 singleHeadMSTPipeline;
        u8  bEnableAudioOverRightPanel; /* NvBool */
    } MST;
    struct {
        u8  bEnhancedFraming;        /* NvBool */
        u32 tuSize;
        u32 waterMark;
        u8  bEnableAudioOverRightPanel; /* NvBool */
    } SST;
} dp_config_stream_t;

/* ------------------------------------------------- core/window method offsets
 * clca7d.h (core, 0x800 head stride) and clca7e.h (window). */
#define CA7D_UPDATE                         0x0200u
#define CA7D_SET_CONTROL                    0x0210u
#define CA7D_SOR_SET_CONTROL(a)             (0x0300u + (a) * 0x20u)
#define   CA7D_SOR_OWNER_MASK_SHIFT         0     /* 7:0 */
#define   CA7D_SOR_PROTOCOL_SHIFT           8     /* 11:8 */
#define CA7D_WINDOW_SET_CONTROL(a)          (0x1000u + (a) * 0x80u)
#define CA7D_HEAD_SET_PROCAMP(a)            (0x2000u + (a) * 0x800u)
#define CA7D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(a) (0x2004u + (a) * 0x800u)
/* EXT_PACKET_WIN 31:26 = NONE (0x3F): no InfoFrame/extended-packet window bound to
 * this head.  nvkms ALWAYS ORs this into CONTROL_OUTPUT_RESOURCE (nvkms-evo4.c:599,
 * clca7d.h:672,705).  Omitting it leaves the field 0 = "packets from WIN0", an
 * incomplete/contradictory head state the GSP supervisor can decline to attach. */
#define CA7D_OUT_RES_EXT_PACKET_WIN_NONE    (0x3Fu << 26)
#define CA7D_HEAD_SET_CONTROL(a)            (0x2008u + (a) * 0x800u)
#define CA7D_HEAD_SET_CONTROL_OUTPUT_SCALER(a) (0x2014u + (a) * 0x800u)
/* LOCK_CHAIN POSITION 3:0 (clca7d.h:955-956).  MANDATORY on ca7d: nvkms emits it
 * immediately after HEAD_SET_CONTROL (EvoSetHeadControlC9, nvkms-evo4.c:568-570).
 * POSITION=0 for a single unlocked head. */
#define CA7D_HEAD_SET_LOCK_CHAIN(a)         (0x2044u + (a) * 0x800u)
/* VIEWPORT_POINT_IN X 14:0, Y 30:16 (clca7d.h:957-959).  REQUIRED alongside
 * VIEWPORT_SIZE_IN; nvkms pushes it in the viewport sequence (nvkms-evo4.c:734-736).
 * (0,0) = scan from the surface origin. */
#define CA7D_HEAD_SET_VIEWPORT_POINT_IN(a)  (0x2048u + (a) * 0x800u)
#define CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(a)     (0x200Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_DITHER_CONTROL(a)     (0x2018u + (a) * 0x800u)
#define CA7D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION(a) (0x201Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_DISPLAY_ID(a,b)       (0x2020u + (a) * 0x800u + (b) * 4u)
#define CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(a) (0x2028u + (a) * 0x800u)
#define CA7D_HEAD_SET_MAX_OUTPUT_SCALE_FACTOR(a) (0x202Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_VIEWPORT_SIZE_IN(a)   (0x204Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_VIEWPORT_SIZE_OUT(a)  (0x2058u + (a) * 0x800u)
#define CA7D_HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(a) (0x205Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_RASTER_SIZE(a)        (0x2064u + (a) * 0x800u)
#define CA7D_HEAD_SET_RASTER_SYNC_END(a)    (0x2068u + (a) * 0x800u)
#define CA7D_HEAD_SET_RASTER_BLANK_END(a)   (0x206Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_RASTER_BLANK_START(a) (0x2070u + (a) * 0x800u)
#define CA7D_HEAD_SET_OVERSCAN_COLOR(a)      (0x2078u + (a) * 0x800u)
#define CA7D_HEAD_SET_FRAME_PACKED_VACTIVE_COLOR(a) (0x207Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_HDMI_CTRL(a)           (0x2080u + (a) * 0x800u)
#define CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI(a) (0x20C0u + (a) * 0x800u)
#define CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI_MAX(a) (0x20C4u + (a) * 0x800u)
#define CA7D_HEAD_SET_SW_SPARE_A(a)          (0x2194u + (a) * 0x800u)
#define CA7D_HEAD_SET_MIN_FRAME_IDLE(a)      (0x2218u + (a) * 0x800u)
#define CA7D_HEAD_SET_DESKTOP_COLOR_ALPHA_RED(a) (0x2220u + (a) * 0x800u)
#define CA7D_HEAD_SET_DESKTOP_COLOR_GREEN_BLUE(a) (0x2224u + (a) * 0x800u)
#define CA7D_HEAD_SET_RASTER_HBLANK_DELAY(a) (0x2364u + (a) * 0x800u)
#define CA7D_HEAD_SET_VSC_SDP_CTRL(a)         (0x2448u + (a) * 0x800u)
#define CA7D_HEAD_SET_VSC_SDP_HEADER(a)       (0x244Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_VSC_SDP_DATA0(a)        (0x2450u + (a) * 0x800u)
#define CA7D_HEAD_SET_DSC_CONTROL(a)          (0x22D4u + (a) * 0x800u)
#define CA7D_HEAD_SET_DSC_PPS_CONTROL(a)      (0x22D8u + (a) * 0x800u)
#define CA7D_HEAD_SET_DSC_PPS_HEAD(a)         (0x22DCu + (a) * 0x800u)
#define CA7D_HEAD_SET_DSC_PPS_DATA0(a)        (0x22E0u + (a) * 0x800u)
#define CA7D_HEAD_SET_HDMI_DSC_HCACTIVE(a)    (0x2368u + (a) * 0x800u)
#define CA7D_HEAD_SET_HDMI_DSC_HCBLANK(a)     (0x236Cu + (a) * 0x800u)
#define CA7D_DSC_CONTROL_ENABLE               (1u << 0)
#define CA7D_DSC_CONTROL_FULL_ICH_PRECISION   (1u << 4)
#define CA7D_DSC_CONTROL_FORCE_ICH_RESET      (1u << 5)
#define CA7D_DSC_CONTROL_FLATNESS_SHIFT       6
#define CA7D_DSC_PPS_ENABLE                   (1u << 0)
#define CA7D_DSC_PPS_LOCATION_VSYNC           (1u << 1)
#define CA7D_DSC_PPS_SIZE_128_BYTES           (0x1fu << 3)
#define CA7D_DSC_PPS_SIZE_HDMI_CVTEM          (0x21u << 3)
/* field values */
#define CA7D_PIXEL_DEPTH_BPP_24_444   0x4u
#define CA7D_STRUCTURE_PROGRESSIVE    0x0u
#define CA7D_PROCAMP_RGB_VESA         0x0u

/* DP 1.3 VSC SDP for the scanout format used here: RGB 4:4:4, sRGB,
 * 8 bits/component, full/VESA range, graphics content.  This is the exact
 * descriptor constructed by nvConstructDpVscSdp() and emitted by
 * EvoSetDpVscSdpCA() for every DP head in the Linux 595 NVKMS driver:
 *   HB0=0, HB1=7(VSC), HB2=5(rev), HB3=19(valid DB bytes)
 *   DB16=RGB+sRGB, DB17=8bpc+VESA, DB18=graphics.
 * Enabling it also makes hardware set MSA MISC1[6], telling the sink to obtain
 * pixel encoding/depth/colorimetry from this packet.  A cold head must not
 * depend on firmware residue for this part of a valid DP video stream. */
#define CA7D_VSC_SDP_CTRL_RGB8_FULL     0x00000041u /* enable + VSYNC location */
#define CA7D_VSC_SDP_HEADER_RGB8_FULL   0x13050700u
#define CA7D_VSC_SDP_DATA4_RGB8_FULL    0x00010100u

#define CA7E_SET_SIZE                 0x0224u
#define CA7E_SET_STORAGE              0x0228u
#define CA7E_SET_PARAMS               0x022Cu
#define CA7E_SET_PLANAR_STORAGE(b)    (0x0230u + (b) * 4u)
#define CA7E_SET_POINT_IN(b)          (0x0290u + (b) * 4u)
#define CA7E_SET_SIZE_IN              0x0298u
#define CA7E_SET_SIZE_OUT             0x02A4u
#define CA7E_SET_PRESENT_CONTROL      0x0308u
#define CA7E_SET_SURFACE_ADDRESS_HI_ISO(b) (0x0658u + (b) * 4u)
#define CA7E_SET_SURFACE_ADDRESS_LO_ISO(b) (0x0670u + (b) * 4u)
#define   CA7E_LO_ISO_TARGET_PHYSICAL_NVM  1u   /* 3:2 */
#define   CA7E_LO_ISO_TARGET_PHYSICAL_PCI  2u
#define   CA7E_LO_ISO_KIND_PITCH           0u   /* 1:1 */
#define   CA7E_LO_ISO_ENABLE               1u   /* 0:0 */
#define CA7E_FORMAT_A8R8G8B8          0xCFu
#define CA7E_FORMAT_X8R8G8B8          0xE6u

/* Core-channel interlock + one-time usage bounds (clca7d.h), and the window's own
 * UPDATE/interlock (clc37e.h base of clca7e.h).  Shutdown uses a core/window
 * interlock; first activation intentionally decouples window flips because that
 * core update also assigns window ownership.  Values are byte-verified. */
#define CA7D_SET_INTERLOCK_FLAGS               0x0218u
#define CA7D_SET_WINDOW_INTERLOCK_FLAGS        0x021Cu
#define CA7D_WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(a) (0x1004u + (a) * 0x80u)
#define CA7D_WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(a) (0x1008u + (a) * 0x80u)
#define CA7D_WINDOW_SET_MAX_INPUT_SCALE_FACTOR(a)     (0x100Cu + (a) * 0x80u)
/* OLUT (output LUT) - MANDATORY on ca7d (headca7d.c:186-215, clca7d.h:749-861).
 * TARGET_PHYSICAL_NVM=1, ENABLE=1 in LO; CONTROL = INTERPOLATE(bit0)=1 |
 * MODE_DIRECT10(3:2)=2 | SIZE(18:8)=1029; FP_NORM_SCALE=0xffffffff. */
#define CA7D_HEAD_SET_SURFACE_ADDRESS_HI_OLUT(a) (0x2158u + (a) * 0x800u)
#define CA7D_HEAD_SET_SURFACE_ADDRESS_LO_OLUT(a) (0x215Cu + (a) * 0x800u)
#define CA7D_HEAD_SET_OLUT_CONTROL(a)            (0x2280u + (a) * 0x800u)
#define CA7D_HEAD_SET_OLUT_FP_NORM_SCALE(a)      (0x2284u + (a) * 0x800u)
#define CA7D_OLUT_CONTROL_VAL  (1u | (2u << 2) | (1029u << 8))   /* INTERP|DIRECT10|SIZE1029 */
#define H_DISP_OLUT  0x00d30000u   /* OLUT VRAM surface handle */
/* Core UPDATE completion notifier (clca7d.h:58-64,198-209).  Lets us CONFIRM the
 * UPDATE actually latched (the display engine writes the notifier on completion),
 * not just that the channel read the pushbuffer. */
#define CA7D_SET_NOTIFIER_CONTROL            0x020Cu
#define   CA7D_NOTIFIER_CONTROL_NOTIFY_ENABLE  (1u << 12)
#define CA7D_SET_SURFACE_ADDRESS_HI_NOTIFIER 0x0260u
#define CA7D_SET_SURFACE_ADDRESS_LO_NOTIFIER 0x0264u
#define H_DISP_NOTIFIER  0x00d40000u
#define CA7D_WINDOW_SET_WINDOW_USAGE_BOUNDS(a)        (0x1010u + (a) * 0x80u)
#define CA7D_WINDOW_SET_PHYSICAL(a)                   (0x1014u + (a) * 0x80u)
#define CA7D_HEAD_SET_HEAD_USAGE_BOUNDS(a)            (0x2030u + (a) * 0x800u)
#define CA7D_HEAD_SET_TILE_MASK(a)                    (0x2060u + (a) * 0x800u)
#define CA7D_TILE_SET_TILE_SIZE(a)                    (0x6000u + (a) * 0x200u)
/* Exact 595 EvoInitWindowMappingCA/C5 defaults recovered from the shipped
 * nv-modeset blob: ILUT + TMO_LUT + 2-tap scaler.  Active windows OR their
 * nvGetMaxPixelsFetchedPerLine(actual viewport width, 1X) into the fetch bound. */
#define CA7D_WINDOW_USAGE_BOUNDS_DEFAULT 0x10110000u
#define CA7D_SCALE_FACTOR_1X             0x0400u
#define CA7D_OUTPUT_SCALER_TAPS_2        0x00000011u
/* Cursor 256x256 + OLUT + 2 output-scaler taps; 1:1 viewport means the
 * UPSCALING_ALLOWED bit (8) is deliberately clear. */
#define CA7D_HEAD_USAGE_BOUNDS_VAL       0x00001014u
#define CA7E_UPDATE                            0x0200u
#define CA7E_SET_INTERLOCK_FLAGS               0x0370u   /* INTERLOCK_WITH_CORE = bit0 */
#define CA7E_SET_WINDOW_INTERLOCK_FLAGS        0x0374u
#define CA7E_SW_SET_MCLK_SWITCH                0x02B4u   /* inherited C57E software method */

/* clc97b.h push header: OPCODE[31:29] | COUNT[27:18] | METHOD_OFFSET[15:2].
 * A 4-aligned method's low two bits are zero, so the word is just
 * (opcode<<29)|(count<<18)|method. */
#define CA_HDR_METHOD  0u
#define CA_HDR(op,count,method) (((op) << 29) | ((count) << 18) | ((method) & 0xFFFCu))

/* Channel USER block bases (r535_chan_user); PUT register = base + NV507C_PUT
 * (0x0), value = dword_count << 2 (PTR field 11:2). */
#define CORE_USER_BASE     0x00680000u
#define WINDOW_USER_BASE(h) (0x00690000u + (h) * 0x1000u)
#define NV507C_PUT         0x0u

/* DISPv05.01 raster-progress counters used by the GB202/GB203 v05.02 HAL too
 * (g_kernel_head_nvoc.c selects kheadGet*Counter_v05_01 for Blackwell). */
#define PDISP_RG_IN_LOADV_COUNTER(h)       (0x00616320u + (h) * 0x800u)
#define PDISP_RG_CRASHLOCK_COUNTER(h)      (0x00616484u + (h) * 0x800u)
#define PDISP_POSTCOMP_LOADV_COUNTER(h)    (0x0061A11Cu + (h) * 0x400u)

/* ARM-vs-ASSY dual-copy state readback (nouveau gv100.c:183-296; ARM = ASSY +
 * 0x8000).  These are direct BAR0 reads of the display engine's live/latched
 * state - the decisive "did the core UPDATE actually promote" probe. */
/* Core-channel ASSY/ARM aperture mirrors the method offsets, so the HEAD stride is
 * 0x800 (same as the modeset methods), NOT 0x400.  ARM = ASSY + 0x8000 (0x68A064 =
 * 0x682064 + 0x8000).  The 0x400 stride was harmless for head0 but wrong for head>0. */
#define PDISP_HEAD_RASTER_ASSY(h)          (0x00682064u + (h) * 0x800u)  /* VTOTAL[31:16]|HTOTAL[15:0] */
#define PDISP_HEAD_RASTER_ARM(h)           (0x0068A064u + (h) * 0x800u)
#define PDISP_HEAD_PCLK_ARM(h)             (0x0068A00Cu + (h) * 0x800u)  /* latched pixel clock Hz (value is an ARM-copy artifact; ignore magnitude) */
#define PDISP_RG_DPCA(h)                   (0x00616330u + (h) * 0x800u)  /* LINE_CNT[15:0]|FRM_CNT[31:16] */
#define PDISP_SF_DP_CTL(h)                 (0x00616550u + (h) * 0x800u)  /* bit27=active-sym enable, [5:0]=wm */
#define PDISP_SOR_OWNER_ASSY(s)            (0x00680300u + (s) * 0x20u)   /* [7:0]=head mask [11:8]=proto */
#define PDISP_SOR_OWNER_ARM(s)             (0x00688300u + (s) * 0x20u)
#define PDISP_SOR_PWR(s)                   (0x0061C004u + (s) * 0x800u)  /* [0]=PU normal, [31]=pending */
#define PDISP_SOR_SEQ_CTL(s)               (0x0061C030u + (s) * 0x800u)  /* [28]=sequencer busy */
#define PDISP_FE_CORE_CHAN_STATE           0x00610630u                   /* [20:16]==0xb quiescent/good */
#define PDISP_FE_SV_INTR                   0x00611C30u                   /* [2:0] supervisor pending */
/* Connector/padlink -> SOR crossbar.  GM200 through GA102 use this unchanged
 * (nouveau gm200_sor_route_{get,set}); displayId BIT(n) selects output n.
 * Low nibble is SOR+1 (zero means unrouted), bit4 selects SOR link B. */
#define PDISP_XBAR_LINK_A(output)          (0x00612308u + (output) * 0x100u)
#define PDISP_XBAR_LINK_B(output)          (0x00612388u + (output) * 0x100u)

/* ------------------------------------------------------------- flat channel */
typedef struct {
    volatile u32 *pb;      /* CPU view of the sysmem pushbuffer   */
    u64           pb_phys; /* its physical address                */
    u32           at;      /* dwords written so far               */
    u32           cap;     /* capacity in dwords                  */
    u32           user;    /* BAR0 USER block base                */
} dchan_t;

/* A secondary output is fully negotiated with RM before any display methods
 * are submitted, then participates in the same first core promotion as the
 * primary.  This mirrors NVKMS's coreInitMethodsPending path: connector/SOR
 * preparation first, one core-only modeset update for every head, and only
 * then independent window flips. */
typedef struct {
    bool ready;
    bool is_hdmi;
    disp_sink_t *sink;
    u32 head;
    u32 window;
    u32 sor;
    u32 protocol;
    u32 link_lanes;
    u32 link_bw;
    bool link_enhanced;
    edid_mode_t mode;
    dp_mode_plan_t dp_plan;
    hdmi_mode_plan_t hdmi_plan;
    dchan_t win;
} disp_secondary_plan_t;

/* Exact public NVC372_CTRL_CMD_IS_MODE_POSSIBLE ABI used by NVKMS before an
 * nvdisplay modeset.  This is not a display method: it asks RM-IMP to validate
 * the complete multi-head fetch/clock/tile proposal and establish the minimum
 * display performance state.  Omitting it left the UEFI-initialized head alive
 * but asked cold secondary heads to promote without the bandwidth/tiling
 * transaction Linux always performs.  NvBool/NvU8 are one byte and these
 * structures deliberately use the compiler's natural ABI, matching FINN. */
typedef struct {
    u8 headIndex;
    u32 maxPixelClkKHz;
    struct { u32 width, height; } rasterSize;
    struct { u32 X, Y; } rasterBlankStart;
    struct { u32 X, Y; } rasterBlankEnd;
    struct { u32 yStart, yEnd; } rasterVertBlank2;
    struct { u32 masterLockMode, masterLockPin, slaveLockMode, slaveLockPin; } control;
    u32 maxDownscaleFactorH, maxDownscaleFactorV;
    u8 outputScalerVerticalTaps;
    u8 bUpscalingAllowedV, bOverfetchEnabled, bLtmAllowed;
    struct { u16 leadingRasterLines, trailingRasterLines; } minFrameIdle;
    u8 lut, cursorSize32p, tileMask, bEnableDsc;
    u16 dscTargetBppX16;
    u32 possibleDscSliceCountMask, maxDscSliceWidth;
    u8 bYUV420Format, bIs2Head1Or, bGetOSLDOutput;
    u8 bDisableMidFrameAndDWCFWatermark;
} c372_imp_head_t;

typedef struct {
    u32 windowIndex, owningHead, formatUsageBound, rotatedFormatUsageBound;
    u32 maxPixelsFetchedPerLine, maxDownscaleFactorH, maxDownscaleFactorV;
    u8 inputScalerVerticalTaps, bUpscalingAllowedV, bOverfetchEnabled;
    u8 lut, tmoLut, surfaceLayout;
} c372_imp_window_t;

typedef struct { u8 numTiles; } c372_tiling_assignment_t;
typedef struct { u8 head, headDscSlices; } c372_tile_entry_t;

typedef struct {
    u32 subdeviceIndex;
    u8 numHeads, numWindows;
    c372_imp_head_t head[NVC372_CTRL_IMP_MAX_HEADS];
    c372_imp_window_t window[NVC372_CTRL_IMP_MAX_WINDOWS];
    u32 options, testMclkFreqKHz;
    u8 bIsPossible;
    u8 bIsOSLDPossible[NVC372_CTRL_IMP_MAX_HEADS];
    u32 minImpVPState, minPState, minRequiredBandwidthKBPS, floorBandwidthKBPS;
    u32 minRequiredHubclkKHz;
    u32 vblankIncreaseInLinesForOSLDMode[NVC372_CTRL_IMP_MAX_HEADS];
    u32 wakeUpRgLineForOSLDMode[NVC372_CTRL_IMP_MAX_HEADS];
    u32 worstCaseMargin, dispClkKHz, numTilingAssignments;
    c372_tiling_assignment_t tilingAssignments[NVC372_CTRL_IMP_MAX_TILES];
    c372_tile_entry_t tileList[NVC372_CTRL_IMP_MAX_TILES];
    char worstCaseDomain[8];
    u8 bUseCachedPerfState;
} c372_is_mode_possible_t;

_Static_assert(sizeof(c372_imp_head_t) == 92, "C372 IMP head ABI");
_Static_assert(sizeof(c372_imp_window_t) == 36, "C372 IMP window ABI");
_Static_assert(sizeof(c372_is_mode_possible_t) == 2048, "C372 IMP params ABI");

static c372_is_mode_possible_t g_imp;
static u32 g_imp_tile_mask[4];
static u32 g_imp_phywin_mask[4];
static u32 g_imp_dsc_slices[4];

static u32 disp_popcount(u32 v) {
    u32 n = 0u;
    while (v) { v &= v - 1u; n++; }
    return n;
}

static u32 disp_take_low_bits(u32 *available, u32 count) {
    u32 taken = 0u;
    while (count && *available) {
        u32 bit = *available & (0u - *available);
        taken |= bit;
        *available &= ~bit;
        count--;
    }
    return count ? 0u : taken;
}

static void disp_imp_add_mode(c372_is_mode_possible_t *imp, u32 head, u32 window,
                              const edid_mode_t *m, bool dsc, u32 dsc_bpp_x16,
                              u32 dsc_slices, u32 dsc_slice_width) {
    if (!m || imp->numHeads >= NVC372_CTRL_IMP_MAX_HEADS ||
        imp->numWindows >= NVC372_CTRL_IMP_MAX_WINDOWS) return;
    c372_imp_head_t *h = &imp->head[imp->numHeads++];
    c372_imp_window_t *w = &imp->window[imp->numWindows++];
    u32 hs = (u32)m->hactive + m->hsync_off;
    u32 vs = (u32)m->vactive + m->vsync_off;
    u32 leading = (u32)m->vtotal - vs;
    u32 trailing = vs - m->vactive;
    if (leading < 2u) leading = 2u;

    h->headIndex = (u8)head;
    h->maxPixelClkKHz = m->pixel_clock_khz;
    h->rasterSize.width = m->htotal;
    h->rasterSize.height = m->vtotal;
    h->rasterBlankEnd.X = (u32)m->htotal - hs - 1u;
    h->rasterBlankEnd.Y = (u32)m->vtotal - vs - 1u;
    h->rasterBlankStart.X = h->rasterBlankEnd.X + m->hactive;
    h->rasterBlankStart.Y = h->rasterBlankEnd.Y + m->vactive;
    h->control.masterLockMode = 0u;
    h->control.masterLockPin = 0x10u;
    h->control.slaveLockMode = 0u;
    h->control.slaveLockPin = 0x10u;
    h->maxDownscaleFactorH = 1024u;
    h->maxDownscaleFactorV = 1024u;
    h->outputScalerVerticalTaps = 2u;
    h->minFrameIdle.leadingRasterLines = (u16)leading;
    h->minFrameIdle.trailingRasterLines = (u16)trailing;
    h->lut = NVC372_CTRL_IMP_LUT_1025;
    h->cursorSize32p = 8u;
    h->bEnableDsc = dsc ? 1u : 0u;
    if (dsc && dsc_slices) {
        h->dscTargetBppX16 = (u16)dsc_bpp_x16;
        h->possibleDscSliceCountMask = 1u << (dsc_slices - 1u);
        h->maxDscSliceWidth = dsc_slice_width;
    }

    w->windowIndex = window;
    w->owningHead = head;
    w->formatUsageBound = NVC372_CTRL_IMP_FORMAT_RGB_PACKED_4_BPP;
    /* nvGetMaxPixelsFetchedPerLine(width, 1X), exact Ampere+ worst case:
     * (((width + 14) * 1024 + 1023) >> 10) + 8 == width + 22. */
    w->maxPixelsFetchedPerLine = (u32)m->hactive + 22u;
    w->maxDownscaleFactorH = 1024u;
    w->maxDownscaleFactorV = 1024u;
    w->inputScalerVerticalTaps = 2u;
    w->lut = NVC372_CTRL_IMP_LUT_1025;
    w->tmoLut = NVC372_CTRL_IMP_LUT_1025;
    /* NVKMS leaves this zero: legacy-equation mode in the C372 ABI. */
    w->surfaceLayout = 0u;
}

static bool disp_imp_validate(nv_card_t *c, nv_rm_t *rm, u32 control_client,
                              u32 rmctrl,
                              const edid_mode_t *primary,
                              const dp_mode_plan_t *primary_plan,
                              const disp_secondary_plan_t *secondary,
                              u32 secondary_count, u32 present_phywin_mask) {
    memset(&g_imp, 0, sizeof g_imp);
    memset(g_imp_tile_mask, 0, sizeof g_imp_tile_mask);
    memset(g_imp_phywin_mask, 0, sizeof g_imp_phywin_mask);
    memset(g_imp_dsc_slices, 0, sizeof g_imp_dsc_slices);
    /* Match NVKMS's normal dGPU modeset path exactly.  Linux only requests
     * NEED_MIN_VPSTATE when the caller explicitly requires boot clocks or is
     * reallocating display bandwidth (the latter is Tegra-only here).  A
     * first modeset on a discrete GPU uses options == 0.  Forcing the option
     * during our 2026-09-05 14:10 hardware boot made RM accept the control
     * transaction but return bIsPossible=0 with every performance/tiling
     * output zero, before any display methods could be submitted. */
    g_imp.options = 0u;
    disp_imp_add_mode(&g_imp, 0u, 0u, primary,
                      primary_plan && primary_plan->enabled,
                      primary_plan ? primary_plan->bits_per_pixel_x16 : 0u,
                      primary_plan ? primary_plan->slice_count : 0u,
                      primary_plan ? primary_plan->slice_width : 0u);
    for (u32 i = 0; i < secondary_count; i++) {
        const disp_secondary_plan_t *s = &secondary[i];
        bool dsc = s->is_hdmi ? s->hdmi_plan.dsc : s->dp_plan.enabled;
        u32 bpp = s->is_hdmi ? s->hdmi_plan.bpp_x16 : s->dp_plan.bits_per_pixel_x16;
        u32 slices = s->is_hdmi ? s->hdmi_plan.slices : s->dp_plan.slice_count;
        u32 sw = s->is_hdmi ? s->hdmi_plan.slice_width : s->dp_plan.slice_width;
        disp_imp_add_mode(&g_imp, s->head, s->window, &s->mode,
                          dsc, bpp, slices, sw);
    }
    /* NVC372 is an NVKMS control object.  The Linux driver allocates it under
     * nvEvoGlobal.clientHandle/pDevEvo->deviceHandle, not under nouveau's
     * dedicated display-engine client.  Keep CA70/NV0073 on the dedicated
     * client, but route this one control through the main RM client which owns
     * RM_DEVICE and the device-wide performance/display state. */
    u32 caller_client = rm->client;
    rm->client = control_client;
    bool ctl_ok = nv_rm_control(c, rm, rmctrl,
                                 NVC372_CTRL_CMD_IS_MODE_POSSIBLE,
                                 &g_imp, sizeof g_imp, NULL, 0, NULL);
    rm->client = caller_client;
    kinfo("nv-disp", "  C372 IMP topology %u head/%u window: control %s possible %u dispclk %u kHz minVP %u minP %u bandwidth %u KB/s assignments %u",
          g_imp.numHeads, g_imp.numWindows, ctl_ok ? "OK" : "REFUSED",
          g_imp.bIsPossible, g_imp.dispClkKHz, g_imp.minImpVPState,
          g_imp.minPState, g_imp.minRequiredBandwidthKBPS,
          g_imp.numTilingAssignments);
    if (ctl_ok) {
        u32 list_at = 0u;
        for (u32 a = 0; a < g_imp.numTilingAssignments &&
                        a < NVC372_CTRL_IMP_MAX_TILES; a++) {
            u32 n = g_imp.tilingAssignments[a].numTiles;
            kinfo("nv-disp", "    IMP assignment %u: %u tile(s)", a, n);
            for (u32 t = 0; t < n && list_at < NVC372_CTRL_IMP_MAX_TILES; t++, list_at++)
                kinfo("nv-disp", "      tile %u -> IMP-head-index %u DSC-slices %u",
                      list_at, g_imp.tileList[list_at].head,
                      g_imp.tileList[list_at].headDscSlices);
        }
    }
    if (!ctl_ok || !g_imp.bIsPossible || !g_imp.numTilingAssignments)
        return false;

    /* Consume the REQUIRED first assignment exactly as EvoIsModePossibleCA.
     * RM returns required tile counts per IMP-array head, not hardware tile
     * identities.  Linux preserves a compatible live assignment, then assigns
     * the lowest compatible free tiles and physical windows.  All our viewports
     * are 1:1 RGB, so either CA tile type is compatible.  Preserve the firmware
     * primary's tile0/phywin0 and allocate the remaining lowest present units. */
    u32 required[4] = { 0u, 0u, 0u, 0u };
    u32 first_tiles = g_imp.tilingAssignments[0].numTiles;
    if (!first_tiles || first_tiles > NVC372_CTRL_IMP_MAX_TILES)
        return false;
    for (u32 i = 0; i < first_tiles; i++) {
        u32 imp_head = g_imp.tileList[i].head;
        if (imp_head >= g_imp.numHeads) return false;
        u32 hw_head = g_imp.head[imp_head].headIndex;
        if (hw_head >= 4u) return false;
        required[hw_head]++;
        if (!g_imp_dsc_slices[hw_head])
            g_imp_dsc_slices[hw_head] = g_imp.tileList[i].headDscSlices;
        else if (g_imp_dsc_slices[hw_head] != g_imp.tileList[i].headDscSlices)
            return false;
    }

    u32 free_tiles = 0xffu;
    u32 free_phywins = present_phywin_mask & 0xffu;
    if (!free_phywins) free_phywins = 0xffu;
    for (u32 i = 0; i < g_imp.numHeads; i++) {
        u32 hw_head = g_imp.head[i].headIndex;
        u32 need = required[hw_head];
        if (!need) return false;
        if (hw_head == 0u && (free_tiles & 1u) && (free_phywins & 1u)) {
            g_imp_tile_mask[0] = 1u;
            g_imp_phywin_mask[0] = 1u;
            free_tiles &= ~1u;
            free_phywins &= ~1u;
            need--;
        }
        u32 tiles = disp_take_low_bits(&free_tiles, need);
        u32 phywins = disp_take_low_bits(&free_phywins, need);
        if (need && (!tiles || !phywins)) return false;
        g_imp_tile_mask[hw_head] |= tiles;
        g_imp_phywin_mask[hw_head] |= phywins;
        if (disp_popcount(g_imp_tile_mask[hw_head]) != required[hw_head] ||
            disp_popcount(g_imp_phywin_mask[hw_head]) != required[hw_head])
            return false;
        kinfo("nv-disp", "    Linux assignment head%u: required %u tile mask %#x phywin mask %#x DSC slices %u",
              hw_head, required[hw_head], g_imp_tile_mask[hw_head],
              g_imp_phywin_mask[hw_head], g_imp_dsc_slices[hw_head]);
    }
    return true;
}

/* DP_CTRL returning success means training was accepted, not that the sink has
 * acquired a valid main-link video stream.  DPCD 0x202..0x204 is the sink's
 * actual receiver verdict: require clock recovery, channel equalization and
 * symbol lock on every negotiated lane, plus inter-lane alignment. */
static bool disp_dp_sink_link_ok(nv_card_t *c, nv_rm_t *rm, u32 objcom,
                                 u32 display_id, u32 lanes, u8 status[3]) {
    memset(status, 0, 3u);
    if (!lanes || lanes > 4u ||
        !nv_dp_aux_read(c, rm, objcom, display_id, 0x202u, status, 3u))
        return false;
    for (u32 lane = 0; lane < lanes; lane++) {
        u32 nibble = (status[lane >> 1] >> ((lane & 1u) * 4u)) & 0xfu;
        if ((nibble & 0x7u) != 0x7u) return false;
    }
    return (status[2] & 0x1u) != 0u;
}

/* Durable diagnostics (survives the klog ring; read by gpu.c's summary).
 * Declared here - before dchan_alloc and the light-up - because those functions
 * record failure detail into it. */
static nv_disp_relight_diag_t g_relight;

/* Pre-allocated scanout VRAM (see nv_disp_prealloc_scanout / nv.h).  Allocated
 * after the one shared real GR context and before codec channels. */
static u64  g_scanout_fb = 0;
static u64  g_scanout_bytes = 0;
static u32  g_scanout_handle = H_DISP_SCANOUT;
static u32  g_scanout_status = 0;
static bool g_scanout_ok = false;
/* Identity OLUT surface (mandatory on ca7d), allocated + filled early alongside
 * the scanout surface so the modeset can point the head's OLUT at it. */
static u64  g_olut_fb = 0;
static bool g_olut_ok = false;
static u64  g_notifier_fb = 0;
static bool g_notifier_ok = false;

static void disp_olut_fill(nv_card_t *c, u64 fb);   /* defined below */

void nv_disp_prealloc_scanout(nv_card_t *c, nv_rm_t *rm) {
    if (g_scanout_ok) return;                      /* already done */
    if (cmdline_has("nodisplay")) return;          /* re-light disabled */
    /* This is a new dedicated surface, so it does not inherit the GOP's padded
     * 4096-pixel pitch.  On the real three-monitor boot that padding plus a 4K
     * floor requested 33,177,600 contiguous ISO bytes and PRIMARY returned
     * NV_ERR_NO_MEMORY, even though every sink is 2560x1440.  Reserve the
     * tightly packed native raster (14,745,600 bytes here).  Once live EDIDs are
     * available the modeset still chooses the greatest exact mode that fits;
     * larger future sinks can use the replacement allocation below. */
    u64 surface_w = g_boot.fb.width;
    u64 surface_h = g_boot.fb.height;
    if (g_boot.display.present && g_boot.display.native_width &&
        g_boot.display.native_height) {
        surface_w = g_boot.display.native_width;
        surface_h = g_boot.display.native_height;
    }
    u64 surface_pitch = ((surface_w * 4ull) + 63ull) & ~63ull;
    u64 bytes = surface_pitch * surface_h;
    g_scanout_ok = nv_vram_alloc_scanout(c, rm, H_DISP_SCANOUT, bytes, &g_scanout_fb);
    g_scanout_status = nv_last_alloc_status;
    g_scanout_bytes = bytes;
    g_scanout_handle = H_DISP_SCANOUT;
    kinfo("nv-disp", "pre-allocated scanout VRAM %s: %llu B @ FB %#llx (status %#x) "
                     "- after shared GR context, before codec channels",
          g_scanout_ok ? "OK" : "FAILED", (unsigned long long)bytes,
          (unsigned long long)g_scanout_fb, g_scanout_status);

    /* Also reserve + fill the identity OLUT (0x20 header + 1025*8 = 8232 B; round
     * to 16 KiB).  Mandatory on ca7d or the head scans out black. */
    if (!g_olut_ok) {
        /* The OLUT is READ by the display engine, and the notifier is WRITTEN by
         * it - both need display-accessible memory, so use the TYPE_PRIMARY
         * scanout allocator, NOT the generic TYPE_IMAGE|NO_SCANOUT one (that placed
         * them where the display cannot reach - the OLUT would read as garbage
         * (black) and the notifier write would never land). */
        g_olut_ok = nv_vram_alloc_scanout(c, rm, H_DISP_OLUT, 0x4000, &g_olut_fb);
        if (g_olut_ok) disp_olut_fill(c, g_olut_fb);
        kinfo("nv-disp", "OLUT surface %s @ FB %#llx", g_olut_ok ? "OK" : "FAILED",
              (unsigned long long)g_olut_fb);
    }
    if (!g_notifier_ok) {
        g_notifier_ok = nv_vram_alloc_scanout(c, rm, H_DISP_NOTIFIER, 0x1000, &g_notifier_fb);
        kinfo("nv-disp", "notifier surface %s @ FB %#llx", g_notifier_ok ? "OK" : "FAILED",
              (unsigned long long)g_notifier_fb);
    }
}

static void dpush(dchan_t *ch, u32 method, u32 count, const u32 *data) {
    if (ch->at + 1u + count > ch->cap) return;
    ch->pb[ch->at++] = CA_HDR(CA_HDR_METHOD, count, method);
    for (u32 i = 0; i < count; i++) ch->pb[ch->at++] = data[i];
}
static void dpush1(dchan_t *ch, u32 method, u32 v) { dpush(ch, method, 1, &v); }

/* Flush the sysmem pushbuffer out of the CPU cache (the display processor reads
 * it over PCIe), then ring PUT so the channel processes what was written. */
static void dkick(nv_card_t *c, dchan_t *ch) {
    u64 first = (u64)(uintptr_t)ch->pb & ~63ull;
    u64 last  = ((u64)(uintptr_t)ch->pb + (u64)ch->at * 4u + 63u) & ~63ull;
    for (u64 line = first; line < last; line += 64)
        __asm__ volatile("clflush (%0)" :: "r"((const void *)(uintptr_t)line) : "memory");
    __asm__ volatile("mfence" ::: "memory");
    nv_wr32(c, ch->user + NV507C_PUT, ch->at << 2);
}

/* CPU -> VRAM write through the Blackwell BAR0 PRAMIN window.  This is the
 * SAME mechanism as nv_chan.c:150-165 (nv_fb_wr32 / nv_fb_rd32), which is
 * file-static there, so we replicate it: NV_XAL_EP_BAR0_WINDOW holds the
 * 64 KiB-aligned VRAM base (fb_offset >> 16) and the 1 MiB PRAMIN aperture at
 * BAR0 + 0x700000 then shows that 64 KiB.  fb_base here is a VRAM-LOCAL byte
 * offset (exactly what nv_vram_alloc returns and what SET_SURFACE_ADDRESS's
 * TARGET_PHYSICAL_NVM consumes), not a CPU/BAR virtual address.  The window is
 * reprogrammed only when the 64 KiB base changes, so a full-screen fill is
 * ~one window write per 64 KiB plus one data write per pixel. */
#define NV_XAL_EP_BAR0_WINDOW  0x0010fd40u
#define NV_BAR0_PRAMIN         0x00700000u
static void disp_vram_wr32(nv_card_t *c, u64 fb, u32 val, u32 *cur_win) {
    u32 win = (u32)(fb >> 16);
    if (win != *cur_win) { nv_wr32(c, NV_XAL_EP_BAR0_WINDOW, win); *cur_win = win; }
    nv_wr32(c, NV_BAR0_PRAMIN + (u32)(fb & 0xFFFFu), val);
}
static u32 disp_vram_rd32(nv_card_t *c, u64 fb) {
    nv_wr32(c, NV_XAL_EP_BAR0_WINDOW, (u32)(fb >> 16));
    return nv_rd32(c, NV_BAR0_PRAMIN + (u32)(fb & 0xFFFFu));
}
static void disp_vram_fill(nv_card_t *c, u64 fb_base, u64 bytes, u32 color) {
    u32 cur_win = 0xFFFFFFFFu;
    for (u64 o = 0; o + 4u <= bytes; o += 4u)
        disp_vram_wr32(c, fb_base + o, color, &cur_win);
}

/* Paint one live-scanout rectangle through CAB5, splitting very wide modes so
 * each repeated source row fits in nv_chan's bounded coherent staging area.
 * Success means the visible pattern was generated by the GPU, not by the CPU
 * or the old firmware framebuffer. */
static bool disp_gpu_rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    const u32 max_chunk_px = 0x7000u / 4u;
    while (w) {
        u32 n = w > max_chunk_px ? max_chunk_px : w;
        if (!nv_chan_fill_scanout((s32)x, (s32)y, (s32)n, (s32)h, color))
            return false;
        x += n;
        w -= n;
    }
    return true;
}

static void disp_pattern_pause(u32 ms) {
    u64 deadline = g_uptime_ms + ms;
    while (g_uptime_ms < deadline) timer_udelay(500);
}

/* Post-modeset patterns on the shared VRAM scanout.  Solids expose stuck color
 * channels, the animated bars exercise all four directions, and the diagonal
 * mosaic mixes primary/secondary colors across both axes. */
static bool disp_run_gpu_pattern_suite(u32 width, u32 height, u32 output_mask) {
    u32 r = 0xFFu << g_boot.fb.red_shift;
    u32 g = 0xFFu << g_boot.fb.green_shift;
    u32 b = 0xFFu << g_boot.fb.blue_shift;
    u32 colors[8] = { r | g | b, r | g, g | b, g, r | b, r, b, 0u };
    static const char *solid_name[4] = { "RED", "GREEN", "BLUE", "WHITE" };
    u32 solids[4] = { r, g, b, r | g | b };
    bool ok = true;

    for (u32 i = 0; i < 4u; i++) {
        ok = disp_gpu_rect(0, 0, width, height, solids[i]) && ok;
        kinfo("nv-disp", "GPU pattern solid %s on output mask %#x",
              solid_name[i], output_mask);
        disp_pattern_pause(1500);
    }

    ok = disp_gpu_rect(0, 0, width, height, 0u) && ok;
    for (u32 i = 0; i < 8u; i++) {
        u32 x0 = (u32)(((u64)width * i) / 8u);
        u32 x1 = (u32)(((u64)width * (i + 1u)) / 8u);
        ok = disp_gpu_rect(x0, 0, x1 - x0, height, colors[i]) && ok;
        disp_pattern_pause(90);
    }
    kinfo("nv-disp", "GPU mixed-color bars LEFT-to-RIGHT on output mask %#x", output_mask);
    disp_pattern_pause(1200);

    ok = disp_gpu_rect(0, 0, width, height, 0u) && ok;
    for (u32 step = 0; step < 8u; step++) {
        u32 i = 7u - step;
        u32 x0 = (u32)(((u64)width * i) / 8u);
        u32 x1 = (u32)(((u64)width * (i + 1u)) / 8u);
        ok = disp_gpu_rect(x0, 0, x1 - x0, height, colors[step]) && ok;
        disp_pattern_pause(90);
    }
    kinfo("nv-disp", "GPU mixed-color bars RIGHT-to-LEFT on output mask %#x", output_mask);
    disp_pattern_pause(1200);

    ok = disp_gpu_rect(0, 0, width, height, 0u) && ok;
    for (u32 i = 0; i < 8u; i++) {
        u32 y0 = (u32)(((u64)height * i) / 8u);
        u32 y1 = (u32)(((u64)height * (i + 1u)) / 8u);
        ok = disp_gpu_rect(0, y0, width, y1 - y0, colors[i]) && ok;
        disp_pattern_pause(90);
    }
    kinfo("nv-disp", "GPU mixed-color bars TOP-to-BOTTOM on output mask %#x", output_mask);
    disp_pattern_pause(1200);

    ok = disp_gpu_rect(0, 0, width, height, 0u) && ok;
    for (u32 step = 0; step < 8u; step++) {
        u32 i = 7u - step;
        u32 y0 = (u32)(((u64)height * i) / 8u);
        u32 y1 = (u32)(((u64)height * (i + 1u)) / 8u);
        ok = disp_gpu_rect(0, y0, width, y1 - y0, colors[step]) && ok;
        disp_pattern_pause(90);
    }
    kinfo("nv-disp", "GPU mixed-color bars BOTTOM-to-TOP on output mask %#x", output_mask);
    disp_pattern_pause(1200);

    for (u32 gy = 0; gy < 6u; gy++) {
        u32 y0 = (u32)(((u64)height * gy) / 6u);
        u32 y1 = (u32)(((u64)height * (gy + 1u)) / 6u);
        for (u32 gx = 0; gx < 8u; gx++) {
            u32 x0 = (u32)(((u64)width * gx) / 8u);
            u32 x1 = (u32)(((u64)width * (gx + 1u)) / 8u);
            ok = disp_gpu_rect(x0, y0, x1 - x0, y1 - y0,
                               colors[(gx + gy) & 7u]) && ok;
        }
    }
    kinfo("nv-disp", "GPU diagonal mixed-color mosaic on output mask %#x: %s",
          output_mask, ok ? "PASS" : "FAILED");
    disp_pattern_pause(2500);
    return ok;
}
/* Build the identity OLUT (output LUT) surface the Blackwell display pipe REQUIRES
 * (headca7d.olut_identity=true; without it the head outputs black).  Layout from
 * headc57d_olut/lut.c: 0x20-byte zeroed VSS header, then 1025 entries at 8-byte
 * stride, each {u16 R, u16 G, u16 B, u16 pad}; entry i (0..1023) = identity ramp
 * value = i*64 (10-bit value in a 16-bit field), entry 1024 replicates 1023.  As
 * u32 words per entry: w0 = R | (G<<16) = v|(v<<16); w1 = B | (pad<<16) = v. */
static void disp_olut_fill(nv_card_t *c, u64 fb) {
    u32 cur_win = 0xFFFFFFFFu;
    for (u32 o = 0; o < 0x20u; o += 4u) disp_vram_wr32(c, fb + o, 0u, &cur_win);
    for (u32 i = 0; i <= 1024u; i++) {
        u32 idx = (i <= 1023u) ? i : 1023u;
        u32 v = idx * 64u;
        u64 e = fb + 0x20u + (u64)i * 8u;
        disp_vram_wr32(c, e + 0u, v | (v << 16), &cur_win);
        disp_vram_wr32(c, e + 4u, v, &cur_win);
    }
}

/* ---------------------------------------------------------- RM control glue */
static bool disp_ctrl(nv_card_t *c, nv_rm_t *rm, u32 object, u32 cmd,
                      void *p, u32 size, const char *what) {
    u32 got = 0;
    /* The NV2080_CTRL_CMD_INTERNAL_DISPLAY_* queries (issued on RM_SUBDEVICE) are
     * privileged like the GR/falcon ones - route them through the RPC-65 internal
     * fallback; the NV0073 DP controls (issued on the objcom handle, which equals
     * NVKM_RM_DISP=0x00730000) go the ordinary way to the objcom object. */
    bool ok = (object == RM_SUBDEVICE)
              ? nv_rm_control_internal(c, rm, cmd, p, size, p, size, &got)
              : nv_rm_control(c, rm, object, cmd, p, size, p, size, &got);
    kinfo("nv-disp", "  %-28s -> %s", what, ok ? "OK" : "refused");
    return ok;
}

/* Read one connected sink exactly as NVKMS does.  The cached read is first so
 * enumerating displays cannot disturb a firmware-lit link.  If the cache is
 * absent or malformed, retry through DDC in RAW mode.  bufferSize is output-only
 * in the 595 ABI; setting it to the capacity is an INVALID_ARGUMENT bug. */
static bool disp_read_sink_edid(nv_card_t *c, nv_rm_t *rm, u32 objcom,
                                u32 display_id, disp_sink_t *sink) {
    static const u32 policies[] = { GET_EDID_COPY_CACHE_YES,
                                    GET_EDID_READ_MODE_RAW };
    for (u32 policy = 0; policy < 2u; policy++) {
        for (u32 attempt = 0; attempt < (policy ? 3u : 1u); attempt++) {
            memset(&g_edid_rpc, 0, sizeof g_edid_rpc);
            g_edid_rpc.displayId = display_id;
            g_edid_rpc.flags = policies[policy];
            bool ok = disp_ctrl(c, rm, objcom,
                                NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2,
                                &g_edid_rpc, sizeof g_edid_rpc,
                                policy ? "GET_EDID_V2 fresh/raw" :
                                         "GET_EDID_V2 cached");
            u32 len = g_edid_rpc.bufferSize;
            if (len > NV0073_GET_EDID_MAX_BYTES)
                len = NV0073_GET_EDID_MAX_BYTES;
            len -= len % EDID_BLOCK_SIZE; /* never expose a partial extension */

            edid_info_t parsed;
            if (ok && len >= EDID_BLOCK_SIZE &&
                edid_parse(g_edid_rpc.edidBuffer, len, &parsed) && parsed.valid) {
                sink->edid = parsed;
                sink->edid_len = len;
                sink->valid = true;
                kinfo("nv-disp", "  displayId %#x EDID: %s %s product %#x, %u B/%d blocks, %d detailed modes (%s)",
                      display_id, parsed.manufacturer,
                      parsed.model[0] ? parsed.model : "(unnamed)", parsed.product_code,
                      len, parsed.block_count, parsed.mode_count,
                      policy ? "fresh DDC raw" : "RM hotplug cache");
                kinfo("nv-disp", "    monitor maxima: resolution %ux%u; refresh %u.%03u Hz at %ux%u; best max-resolution pair %ux%u @ %u.%03u Hz",
                      parsed.highest_resolution.hactive,
                      parsed.highest_resolution.vactive,
                      parsed.highest_refresh.refresh_mhz / 1000u,
                      parsed.highest_refresh.refresh_mhz % 1000u,
                      parsed.highest_refresh.hactive,
                      parsed.highest_refresh.vactive,
                      parsed.highest_resolution_refresh.hactive,
                      parsed.highest_resolution_refresh.vactive,
                      parsed.highest_resolution_refresh.refresh_mhz / 1000u,
                      parsed.highest_resolution_refresh.refresh_mhz % 1000u);
                return true;
            }
            if (policy && attempt + 1u < 3u) {
                u64 dl = g_uptime_ms + 20u;
                while (g_uptime_ms < dl) timer_udelay(500);
            }
        }
    }
    kwarn("nv-disp", "  displayId %#x: connected but no valid EDID from cache or DDC",
          display_id);
    return false;
}

static bool disp_mode_fits_uncompressed_dp(const edid_mode_t *m,
                                            u32 link_bw, u32 lanes) {
    if (!m->pixel_clock_khz || !link_bw || !lanes) return false;
    /* Same 8b/10b payload test used by disp_dp_watermark_sst: 24-bit RGB must
     * fit below 8 * symbol-clock * lane-count.  Equality leaves no blanking
     * margin and is deliberately rejected. */
    u64 pixels_hz = (u64)m->pixel_clock_khz * 1000ull;
    u64 symbols_hz = (u64)link_bw * 27000ull * 1000ull;
    return pixels_hz * 24ull < 8ull * symbols_hz * (u64)lanes;
}

/* Return the next lower *advertised* refresh at the current maximum
 * resolution.  This is used only after RM-IMP rejects the complete topology;
 * it never invents a timing and never silently drops panel resolution. */
static bool disp_pick_next_lower_dp_mode(const edid_info_t *ed,
                                         const edid_mode_t *current,
                                         u32 link_bw, u32 lanes,
                                         u64 pitch, u64 surface_bytes,
                                         edid_mode_t *out) {
    bool found = false;
    u32 best_refresh = 0u;
    if (!ed || !current || !current->refresh_mhz) return false;
    for (int i = 0; i < ed->mode_count; i++) {
        const edid_mode_t *m = &ed->modes[i];
        if (m->hactive != current->hactive || m->vactive != current->vactive ||
            !m->htotal || !m->vtotal || !m->hsync_w || !m->vsync_w ||
            (m->flags & 0x80u) || m->refresh_mhz >= current->refresh_mhz)
            continue;
        if ((u64)m->hactive * 4ull > pitch ||
            pitch * (u64)m->vactive > surface_bytes ||
            !disp_mode_fits_uncompressed_dp(m, link_bw, lanes))
            continue;
        if (!found || m->refresh_mhz > best_refresh) {
            *out = *m;
            best_refresh = m->refresh_mhz;
            found = true;
        }
    }
    return found;
}

static u32 disp_dsc_max_slices(u32 mask) {
    static const u8 counts[] = { 1, 2, 0, 4, 6, 8, 10, 12, 16, 20, 24 };
    u32 best = 0;
    for (u32 bit = 0; bit < sizeof counts; bit++)
        if ((mask & (1u << bit)) && counts[bit] > best) best = counts[bit];
    return best;
}

static bool disp_read_dp_dsc_caps(nv_card_t *c, nv_rm_t *rm, u32 objcom,
                                  u32 display_id, dp_sink_dsc_t *out) {
    u8 d[16], fec = 0;
    memset(out, 0, sizeof *out);
    if (!nv_dp_aux_read(c, rm, objcom, display_id, 0x60u, d, sizeof d) ||
        !nv_dp_aux_read(c, rm, objcom, display_id, 0x90u, &fec, 1u))
        return false;
    if (!(d[0] & 1u) || !(fec & 1u)) return false;
    out->revision_major = d[1] & 0xfu;
    out->revision_minor = d[1] >> 4;
    u32 block_kb = 1u << ((d[2] & 3u) * 2u);
    out->rc_buffer_kb = block_kb * (u32)d[3];
    out->slice_mask = (u32)d[4] | ((u32)(d[13] & 7u) << 8);
    out->max_slices = disp_dsc_max_slices(out->slice_mask);
    out->max_slice_width = (u32)d[12] * 320u;
    u32 lbd = d[5] & 0xfu;
    out->line_buffer_depth = lbd == 8u ? 8u : lbd + 9u;
    out->block_prediction = (d[6] & 1u) != 0;
    out->max_bpp_x16 = (u32)d[7] | ((u32)(d[8] & 3u) << 8);
    out->color_formats = d[9] & 0x1fu;
    out->color_depths = (d[10] >> 1) & 7u;
    out->peak_throughput0 = d[11] & 0xfu;
    out->peak_throughput1 = d[11] >> 4;
    out->bpp_precision = 1u << (d[15] & 7u);
    out->fec_capable = true;
    out->valid = out->revision_major == 1u && out->revision_minor >= 1u &&
                 out->max_slices && out->max_slice_width &&
                 (out->color_formats & DSC_DECODER_COLOR_FORMAT_RGB) &&
                 (out->color_depths & DSC_DECODER_COLOR_DEPTH_CAPS_8_BITS);
    kinfo("nv-disp", "  DP DSC sink %#x: v%u.%u slices %#x/max%u width%u bppMax%u/16 precision%u FEC %d",
          display_id, out->revision_major, out->revision_minor, out->slice_mask,
          out->max_slices, out->max_slice_width, out->max_bpp_x16,
          out->bpp_precision, (int)out->fec_capable);
    return out->valid;
}

static bool disp_make_dp_dsc_plan(nv_card_t *c, nv_rm_t *rm, u32 objcom,
                                  u32 display_id, u32 head,
                                  const edid_mode_t *m, u32 link_bw, u32 lanes,
                                  bool enhanced, const dp_get_caps_t *gpu,
                                  const dp_sink_dsc_t *sink,
                                  dp_mode_plan_t *plan) {
    memset(plan, 0, sizeof *plan);
    if (!gpu->bFECSupported || !gpu->DSC.bDscSupported ||
        !(gpu->DSC.encoderColorFormatMask & DSC_ENCODER_COLOR_FORMAT_RGB) ||
        !sink->valid || !sink->fec_capable || gpu->DSC.bitsPerPixelPrecision < 1u ||
        gpu->DSC.bitsPerPixelPrecision > 5u)
        return false;

    DSC_INFO di; memset(&di, 0, sizeof di);
    di.sinkCaps.decoderColorFormatMask = sink->color_formats;
    di.sinkCaps.bitsPerPixelPrecision = sink->bpp_precision;
    di.sinkCaps.maxSliceWidth = sink->max_slice_width;
    di.sinkCaps.maxNumHztSlices = sink->max_slices;
    di.sinkCaps.sliceCountSupportedMask = sink->slice_mask;
    di.sinkCaps.lineBufferBitDepth = sink->line_buffer_depth;
    di.sinkCaps.decoderColorDepthCaps = sink->color_depths;
    di.sinkCaps.decoderColorDepthMask = sink->color_depths;
    di.sinkCaps.algorithmRevision.versionMajor = sink->revision_major;
    di.sinkCaps.algorithmRevision.versionMinor = sink->revision_minor;
    di.sinkCaps.bBlockPrediction = sink->block_prediction ? NV_TRUE : NV_FALSE;
    di.sinkCaps.peakThroughputMode0 = sink->peak_throughput0;
    di.sinkCaps.peakThroughputMode1 = sink->peak_throughput1;
    di.sinkCaps.maxBitsPerPixelX16 = sink->max_bpp_x16;
    di.gpuCaps.encoderColorFormatMask = gpu->DSC.encoderColorFormatMask;
    di.gpuCaps.lineBufferSize = gpu->DSC.lineBufferSizeKB;
    di.gpuCaps.bitsPerPixelPrecision = 1u << (gpu->DSC.bitsPerPixelPrecision - 1u);
    di.gpuCaps.maxNumHztSlices = gpu->DSC.maxNumHztSlices;
    di.gpuCaps.lineBufferBitDepth = gpu->DSC.lineBufferBitDepth;

    MODESET_INFO mi; memset(&mi, 0, sizeof mi);
    mi.pixelClockHz = (u64)m->pixel_clock_khz * 1000ull;
    mi.activeWidth = m->hactive; mi.activeHeight = m->vactive;
    mi.bitsPerComponent = 8u; mi.colorFormat = NVT_COLOR_FORMAT_RGB;
    WAR_DATA war; memset(&war, 0, sizeof war);
    war.connectorType = DSC_DP;
    war.dpData.linkRateHz = (u64)link_bw * 270000000ull;
    war.dpData.laneCount = lanes; war.dpData.dpMode = DSC_DP_SST;
    war.dpData.hBlank = m->htotal - m->hactive;

    /* DP 1.4 requires budgeting the 3% FEC symbol overhead before PPS bpp
     * selection (ctrl0073dp.h's ENABLE_FEC contract). */
    u64 available = war.dpData.linkRateHz * (u64)lanes * 8ull / 10ull;
    available = available * 97ull / 100ull;
    u32 possible_slices = 0;
    NVT_STATUS st = DSC_GeneratePPSWithSliceCountMask(&di, &mi, &war, available,
                                                       plan->pps,
                                                       &plan->bits_per_pixel_x16,
                                                       &possible_slices);
    if (st != NVT_STATUS_SUCCESS) return false;
    const u8 *p = (const u8 *)plan->pps;
    plan->slice_height = ((u32)p[10] << 8) | p[11];
    plan->slice_width = ((u32)p[12] << 8) | p[13];
    if (!plan->slice_width || !plan->slice_height) return false;
    plan->slice_count = (m->hactive + plan->slice_width - 1u) / plan->slice_width;

    dp_imp_t imp; memset(&imp, 0, sizeof imp);
    imp.displayId = display_id; imp.headIndex = head;
    imp.linkConfig.linkRate10M = link_bw * 27u;
    imp.linkConfig.laneCount = lanes;
    imp.linkConfig.bEnhancedFraming = enhanced ? 1u : 0u;
    imp.linkConfig.bFECEnabled = 1u;
    imp.modesetInfo.rasterWidth = m->htotal;
    imp.modesetInfo.rasterHeight = m->vtotal;
    imp.modesetInfo.surfaceWidth = m->hactive;
    imp.modesetInfo.surfaceHeight = m->vactive;
    u32 hs = m->hactive + m->hsync_off;
    imp.modesetInfo.rasterBlankEndX = m->htotal - hs - 1u;
    imp.modesetInfo.rasterBlankStartX = imp.modesetInfo.rasterBlankEndX + m->hactive;
    imp.modesetInfo.depth = plan->bits_per_pixel_x16;
    imp.modesetInfo.pixelFrequencyKHz = m->pixel_clock_khz;
    imp.modesetInfo.bitsPerComponent = 8u;
    imp.modesetInfo.colorFormat = 0u; /* dpColorFormat_RGB */
    imp.modesetInfo.bDSCEnabled = 1u;
    imp.dscInfo.sliceCount = plan->slice_count;
    imp.dscInfo.sliceWidth = plan->slice_width;
    imp.dscInfo.sliceHeight = plan->slice_height;
    imp.dscInfo.dscVersionMajor = sink->revision_major;
    imp.dscInfo.dscVersionMinor = sink->revision_minor;
    if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_CALCULATE_DP_IMP,
                   &imp, sizeof imp, "CALCULATE_DP_IMP (DSC)") ||
        !imp.watermark.bIsModePossible)
        return false;
    plan->wm = imp.watermark.waterMark; plan->tu = imp.watermark.tuSize;
    plan->hblank_sym = imp.watermark.hBlankSym;
    plan->vblank_sym = imp.watermark.vBlankSym;
    plan->enabled = true;
    return true;
}

/* "Maximum monitor mode" is not a hardcoded refresh value.  Rank exact
 * detailed timings by pixel area, then refresh, while enforcing what this
 * driver can genuinely transmit today: progressive, uncompressed RGB8, the
 * negotiated classic-DP link, and the allocated linear scanout surface. */
static bool disp_pick_best_mode(const edid_info_t *ed, u32 link_bw, u32 lanes,
                                u64 pitch, u64 surface_bytes, edid_mode_t *out) {
    bool found = false;
    u64 best_area = 0;
    u32 best_refresh = 0;
    for (int i = 0; i < ed->mode_count; i++) {
        const edid_mode_t *m = &ed->modes[i];
        if (!m->hactive || !m->vactive || !m->htotal || !m->vtotal ||
            !m->hsync_w || !m->vsync_w || (m->flags & 0x80u))
            continue; /* malformed or interlaced */
        if ((u64)m->hactive * 4ull > pitch ||
            pitch * (u64)m->vactive > surface_bytes)
            continue;
        if (!disp_mode_fits_uncompressed_dp(m, link_bw, lanes))
            continue; /* this helper is intentionally the uncompressed pre-pass */
        u64 area = (u64)m->hactive * (u64)m->vactive;
        if (!found || area > best_area ||
            (area == best_area && m->refresh_mhz > best_refresh)) {
            *out = *m;
            best_area = area;
            best_refresh = m->refresh_mhz;
            found = true;
        }
    }
    return found;
}

static bool disp_pick_best_dp_mode(nv_card_t *c, nv_rm_t *rm, u32 objcom,
                                   u32 display_id, u32 head,
                                   const edid_info_t *ed, u32 link_bw, u32 lanes,
                                   bool enhanced, u64 pitch, u64 surface_bytes,
                                   const dp_get_caps_t *gpu,
                                   const dp_sink_dsc_t *sink_dsc,
                                   edid_mode_t *out, dp_mode_plan_t *out_plan) {
    bool found = false;
    u64 best_area = 0;
    u32 best_refresh = 0;
    for (int i = 0; i < ed->mode_count; i++) {
        const edid_mode_t *m = &ed->modes[i];
        if (!m->hactive || !m->vactive || !m->htotal || !m->vtotal ||
            !m->hsync_w || !m->vsync_w || (m->flags & 0x80u) ||
            (u64)m->hactive * 4ull > pitch ||
            pitch * (u64)m->vactive > surface_bytes)
            continue;
        u64 area = (u64)m->hactive * m->vactive;
        if (found && (area < best_area ||
            (area == best_area && m->refresh_mhz <= best_refresh)))
            continue;
        dp_mode_plan_t candidate; memset(&candidate, 0, sizeof candidate);
        if (!disp_mode_fits_uncompressed_dp(m, link_bw, lanes) &&
            !disp_make_dp_dsc_plan(c, rm, objcom, display_id, head, m,
                                   link_bw, lanes, enhanced, gpu, sink_dsc,
                                   &candidate))
            continue;
        *out = *m; *out_plan = candidate;
        best_area = area; best_refresh = m->refresh_mhz; found = true;
    }
    return found;
}

static void hdmi_frl_rate_data(u32 rate, u32 *lanes, u32 *gbps) {
    static const u8 l[] = { 0, 3, 3, 4, 4, 4, 4 };
    static const u8 g[] = { 0, 3, 6, 6, 8, 10, 12 };
    if (rate > 6u) rate = 0u;
    *lanes = l[rate]; *gbps = g[rate];
}

static u32 hdmi_slice_mask(u32 max_slices) {
    u32 m = 0;
    if (max_slices >= 1u) m |= DSC_DECODER_SLICES_PER_SINK_1;
    if (max_slices >= 2u) m |= DSC_DECODER_SLICES_PER_SINK_2;
    if (max_slices >= 4u) m |= DSC_DECODER_SLICES_PER_SINK_4;
    if (max_slices >= 8u) m |= DSC_DECODER_SLICES_PER_SINK_8;
    if (max_slices >= 12u) m |= DSC_DECODER_SLICES_PER_SINK_12;
    if (max_slices >= 16u) m |= DSC_DECODER_SLICES_PER_SINK_16;
    return m;
}

static void hdmi_fill_capacity_input(hdmi_frl_capacity_t *q,
                                     const edid_info_t *ed,
                                     const edid_mode_t *m, u32 rate) {
    memset(q, 0, sizeof *q);
    hdmi_frl_rate_data(rate, &q->input.numLanes, &q->input.frlBitRateGbps);
    q->input.pclk10KHz = m->pixel_clock_khz / 10u;
    q->input.hTotal = m->htotal; q->input.hActive = m->hactive;
    q->input.bpc = 8u; q->input.pixelPacking = 0u; /* HDMI RGB */
    q->input.compressionInfo.dscTotalChunkKBytes =
        (u32)ed->hdmi_forum.dsc_total_chunk_kbytes * 1024u;
}

static bool disp_make_hdmi_dsc_pps(const edid_info_t *ed,
                                   const edid_mode_t *m,
                                   const dp_get_caps_t *gpu,
                                   const hdmi_frl_capacity_t *q,
                                   hdmi_mode_plan_t *plan) {
    DSC_INFO di; memset(&di, 0, sizeof di);
    di.gpuCaps.encoderColorFormatMask = gpu->DSC.encoderColorFormatMask;
    di.gpuCaps.lineBufferSize = gpu->DSC.lineBufferSizeKB;
    di.gpuCaps.bitsPerPixelPrecision = gpu->DSC.bitsPerPixelPrecision >= 1u &&
        gpu->DSC.bitsPerPixelPrecision <= 5u ?
        1u << (gpu->DSC.bitsPerPixelPrecision - 1u) : 0u;
    di.gpuCaps.maxNumHztSlices = gpu->DSC.maxNumHztSlices;
    di.gpuCaps.lineBufferBitDepth = gpu->DSC.lineBufferBitDepth;
    di.sinkCaps.decoderColorFormatMask = DSC_DECODER_COLOR_FORMAT_RGB |
        DSC_DECODER_COLOR_FORMAT_Y_CB_CR_444 |
        DSC_DECODER_COLOR_FORMAT_Y_CB_CR_SIMPLE_422 |
        DSC_DECODER_COLOR_FORMAT_Y_CB_CR_NATIVE_422;
    di.sinkCaps.bitsPerPixelPrecision = DSC_BITS_PER_PIXEL_PRECISION_1_16;
    di.sinkCaps.maxSliceWidth = 2720u;
    di.sinkCaps.maxNumHztSlices = ed->hdmi_forum.dsc_max_slices;
    di.sinkCaps.sliceCountSupportedMask = hdmi_slice_mask(ed->hdmi_forum.dsc_max_slices);
    di.sinkCaps.lineBufferBitDepth = 13u;
    di.sinkCaps.decoderColorDepthCaps = DSC_DECODER_COLOR_DEPTH_CAPS_8_BITS;
    di.sinkCaps.decoderColorDepthMask = DSC_DECODER_COLOR_DEPTH_CAPS_8_BITS;
    di.sinkCaps.algorithmRevision.versionMajor = 1u;
    di.sinkCaps.algorithmRevision.versionMinor = 2u;
    di.sinkCaps.bBlockPrediction = NV_TRUE;
    di.sinkCaps.peakThroughputMode0 = ed->hdmi_forum.dsc_max_pclk_per_slice_mhz >= 400u ?
        DSC_DECODER_PEAK_THROUGHPUT_MODE0_400 : DSC_DECODER_PEAK_THROUGHPUT_MODE0_340;
    di.sinkCaps.maxBitsPerPixelX16 = ed->hdmi_forum.dsc_all_bpp ? 384u : 192u;
    di.forcedDscParams.sliceWidth = q->input.compressionInfo.sliceWidth;
    di.forcedDscParams.dscRevision.versionMajor = 1u;
    di.forcedDscParams.dscRevision.versionMinor = 2u;
    MODESET_INFO mi; memset(&mi, 0, sizeof mi);
    mi.pixelClockHz = (u64)m->pixel_clock_khz * 1000ull;
    mi.activeWidth = m->hactive; mi.activeHeight = m->vactive;
    mi.bitsPerComponent = 8u; mi.colorFormat = NVT_COLOR_FORMAT_RGB;
    WAR_DATA war; memset(&war, 0, sizeof war); war.connectorType = DSC_HDMI;
    DSC_GENERATE_PPS_OPAQUE_WORKAREA scratch;
    u32 bpp = plan->bpp_x16;
    u64 available = (u64)q->input.frlBitRateGbps * q->input.numLanes * 1000000000ull;
    return DSC_GeneratePPS(&di, &mi, &war, available, &scratch,
                           plan->pps, &bpp) == NVT_STATUS_SUCCESS &&
           bpp == plan->bpp_x16;
}

static bool disp_pick_best_hdmi_mode(nv_card_t *c, nv_rm_t *rm, u32 objcom,
                                     u32 display_id, const edid_info_t *ed,
                                     u64 pitch, u64 surface_bytes,
                                     const dp_get_caps_t *gpu, u32 max_frl,
                                     u32 max_dsc_frl,
                                     edid_mode_t *out, hdmi_mode_plan_t *plan) {
    bool found = false; u64 best_area = 0; u32 best_refresh = 0;
    for (int i = 0; i < ed->mode_count; i++) {
        const edid_mode_t *m = &ed->modes[i];
        if (!m->hactive || !m->vactive || !m->htotal || !m->vtotal ||
            !m->hsync_w || !m->vsync_w || (m->flags & 0x80u) ||
            (u64)m->hactive * 4ull > pitch || pitch * (u64)m->vactive > surface_bytes)
            continue;
        u64 area = (u64)m->hactive * m->vactive;
        if (found && (area < best_area ||
            (area == best_area && m->refresh_mhz <= best_refresh))) continue;
        hdmi_mode_plan_t p; memset(&p, 0, sizeof p); p.frl_rate = max_frl;
        hdmi_frl_capacity_t q; hdmi_fill_capacity_input(&q, ed, m, max_frl);
        q.cmd = 1u; /* UNCOMPRESSED_VIDEO */
        bool transport = disp_ctrl(c, rm, objcom,
            NV0073_CTRL_CMD_SPECIFIC_GET_HDMI_FRL_CAPACITY_COMPUTATION,
            &q, sizeof q, "HDMI FRL capacity (uncompressed)") &&
            q.result.isVideoTransportSupported;
        if (!transport && ed->hdmi_forum.dsc_1p2 && gpu->DSC.bDscSupported &&
            (gpu->DSC.encoderColorFormatMask & DSC_ENCODER_COLOR_FORMAT_RGB)) {
            if (!max_dsc_frl) continue;
            hdmi_fill_capacity_input(&q, ed, m, max_dsc_frl);
            p.frl_rate = max_dsc_frl;
            q.dsc.maxSliceCount = gpu->DSC.maxNumHztSlices < ed->hdmi_forum.dsc_max_slices ?
                                  gpu->DSC.maxNumHztSlices : ed->hdmi_forum.dsc_max_slices;
            q.dsc.maxSliceWidth = 5120u; q.cmd = 6u; /* IS_FRL_DSC_POSSIBLE */
            if (q.dsc.maxSliceCount && disp_ctrl(c, rm, objcom,
                NV0073_CTRL_CMD_SPECIFIC_GET_HDMI_FRL_CAPACITY_COMPUTATION,
                &q, sizeof q, "HDMI FRL DSC feasibility") && q.dsc.bIsDSCPossible) {
                u32 max_bpp = ed->hdmi_forum.dsc_all_bpp ? 383u : 192u;
                u32 step = 16u;
                for (u32 bpp = max_bpp; bpp >= 128u;) {
                    q.cmd = 2u; q.input.compressionInfo.bppTargetx16 = bpp;
                    if (disp_ctrl(c, rm, objcom,
                        NV0073_CTRL_CMD_SPECIFIC_GET_HDMI_FRL_CAPACITY_COMPUTATION,
                        &q, sizeof q, "HDMI FRL capacity (DSC)") &&
                        q.result.isVideoTransportSupported) {
                        if (step == 16u && bpp != max_bpp) {
                            bpp += 15u; step = 1u;
                            continue;
                        }
                        p.dsc = true; p.bpp_x16 = q.result.bppTargetx16 ?
                            q.result.bppTargetx16 : bpp;
                        p.slices = q.input.compressionInfo.hSlices;
                        p.slice_width = q.input.compressionInfo.sliceWidth;
                        p.hactive_bytes = q.result.hcActiveBytes;
                        p.hactive_tribytes = q.result.hcActiveTriBytes;
                        p.hblank_tribytes = q.result.hcBlankTriBytes;
                        transport = disp_make_hdmi_dsc_pps(ed, m, gpu, &q, &p);
                        break;
                    }
                    if (bpp < 128u + step) break;
                    bpp -= step;
                }
            }
        }
        if (!transport) continue;
        *out = *m; *plan = p; best_area = area;
        best_refresh = m->refresh_mhz; found = true;
    }
    return found;
}

static bool disp_pick_best_tmds_mode(const edid_info_t *ed, u32 max_tmds_khz,
                                     u64 pitch, u64 surface_bytes,
                                     edid_mode_t *out) {
    bool found = false; u64 best_area = 0u; u32 best_refresh = 0u;
    for (int i = 0; i < ed->mode_count; i++) {
        const edid_mode_t *m = &ed->modes[i];
        if (!m->hactive || !m->vactive || !m->htotal || !m->vtotal ||
            !m->hsync_w || !m->vsync_w || (m->flags & 0x80u) ||
            m->pixel_clock_khz > max_tmds_khz ||
            (u64)m->hactive * 4ull > pitch ||
            pitch * (u64)m->vactive > surface_bytes)
            continue;
        u64 area = (u64)m->hactive * m->vactive;
        if (!found || area > best_area ||
            (area == best_area && m->refresh_mhz > best_refresh)) {
            *out = *m; best_area = area; best_refresh = m->refresh_mhz;
            found = true;
        }
    }
    return found;
}

/* Allocate a sysmem pushbuffer page for a display channel.
 *
 * dma_alloc_pages() returns ordinary CPU write-back system RAM.  That is the
 * coherent HOST target in Nouveau's memory model, not its NCOH target:
 * nv50_dmac_create() requests NVIF_MEM_COHERENT and r535_disp_chan_set_pushbuf()
 * consequently sends cacheSnoop=1.  Advertising this page as non-snooped lets
 * PDISP retain the zero-filled cache line it saw while acquiring the channel.
 * PUT/GET can then advance normally while the engine parses zeros (NOPs), which
 * is exactly the otherwise paradoxical on-silicon signature we saw: GET==PUT,
 * no FE exception, but no core method reaches ASSY and the notifier is untouched.
 * Keep the explicit CLFLUSH in dkick as an ordering/writeback belt-and-braces,
 * but describe the host memory truthfully so device-side caching is coherent. */
static bool dchan_alloc(nv_card_t *c, nv_rm_t *rm, dchan_t *ch, u32 oclass,
                        u32 inst, u32 user_base, const char *name) {
    u64 phys = 0;
    void *va = dma_alloc_pages(1, &phys);         /* one 4 KiB page (PUT is 11:2 = 4 KiB) */
    if (!va) { kerr("nv-disp", "%s: no pushbuffer", name); return false; }
    memset(va, 0, PAGE_SIZE);
    ch->pb = (volatile u32 *)va; ch->pb_phys = phys; ch->at = 0;
    ch->cap = PAGE_SIZE / 4u; ch->user = user_base;

    /* 1. Register the pushbuffer address BEFORE allocating the channel object -
     *    the ordering r535_dmac_init enforces; get it backwards and GSP-RM either
     *    rejects the alloc or never reads the pushbuffer. */
    disp_channel_pushbuffer_t pbp; memset(&pbp, 0, sizeof pbp);
    pbp.addressSpace = DISP_ADDR_SYSMEM;
    pbp.physicalAddr = phys;
    pbp.limit = PAGE_SIZE - 1u;
    pbp.cacheSnoop = DISP_CACHE_SNOOP_COHERENT;
    pbp.hclass = oclass;
    pbp.channelInstance = inst;
    pbp.valid = 1;
    if (!disp_ctrl(c, rm, RM_SUBDEVICE, NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER,
                   &pbp, sizeof pbp, "channel pushbuffer")) {
        g_relight.chan_fail_stage = 1; g_relight.chan_pb_status = nv_last_control_status;
        kerr("nv-disp", "%s: CHANNEL_PUSHBUFFER refused (status %#x)", name, nv_last_control_status);
        return false;
    }

    /* 2. Allocate the channel object under the disp root. */
    vaio_channeldma_alloc_t args; memset(&args, 0, sizeof args);
    args.channelInstance = inst;
    args.offset = 0;
    if (!nv_rm_alloc(c, rm, H_DISP_ROOT, (oclass << 16) | inst, oclass, &args, sizeof args)) {
        g_relight.chan_fail_stage = 2; g_relight.chan_alloc_status = nv_last_alloc_status;
        kerr("nv-disp", "%s: channel alloc refused (status %#x)", name, nv_last_alloc_status);
        return false;
    }
    kinfo("nv-disp", "%s channel open (class %04x inst %u, USER %#x)", name, oclass, inst, user_base);
    return true;
}

/* Map the maxLinkRate cap (1..4) to the DP_DATA SET_LINK_BW code. */
static u32 dp_link_bw_code(u32 max_link_rate) {
    switch (max_link_rate) {
        case 1: return DP_LINK_BW_1_62;
        case 2: return DP_LINK_BW_2_70;
        case 3: return DP_LINK_BW_5_40;
        case 4: return DP_LINK_BW_8_10;
        default: return DP_LINK_BW_2_70;   /* conservative fallback */
    }
}

/* Reproduce nv50_sor_dp_watermark_sst (dispnv50/disp.c:1607-1741) verbatim.  In
 * the GSP model the driver MUST push these with DP_CONFIG_STREAM bEnableOverride=1;
 * with override=0 the stream-formatter watermark/hblank/vblank stay at reset(0)
 * and the SOR sends a trained link carrying NO active symbols = black screen.
 * bpc=8 (24bpp, depth=24).  link_bw_code*27000 = link symbol clock in kHz. */
static bool disp_dp_watermark_sst(u32 link_bw_code, u32 lanes, u32 pclk_khz,
                                  u32 hactive, u32 htotal, bool enhanced,
                                  u32 *out_wm, u32 *out_hblank, u32 *out_vblank) {
    const u64 PF = 100000ull;
    u32 tuSize = 64, depth = 8u * 3u;
    u32 watermarkAdjust = 2, watermarkMinimum = 20;   /* DP_CONFIG_WATERMARK_* */
    u64 minRate = (u64)link_bw_code * 27000ull * 1000ull;   /* code*27000 kHz -> Hz */
    u64 pixelClockHz = (u64)pclk_khz * 1000ull;
    u32 numLanes = lanes ? lanes : 1u;
    u32 surfaceWidth = hactive, rasterWidth = htotal;

    if (!minRate || !pixelClockHz) return false;
    if ((pixelClockHz * depth) >= (8ull * minRate * numLanes)) return false;

    u64 ratioF = (pixelClockHz * depth * PF) / (8ull * minRate * numLanes);
    if (PF < ratioF) return false;
    u64 watermarkF = (ratioF * tuSize * (PF - ratioF)) / PF;
    u32 waterMark = (u32)(watermarkAdjust +
        (((2ull * ((u64)depth * PF / (8ull * numLanes))) + watermarkF) / PF));

    u32 numSymbolsPerLine = (u32)(((u64)surfaceWidth * depth) / (8ull * numLanes));
    if (waterMark > 39u || waterMark > numSymbolsPerLine) return false;
    if (waterMark < watermarkMinimum) waterMark = watermarkMinimum;

    u32 BlankingBits = 3u*8u*numLanes + (enhanced ? 3u*8u*numLanes : 0u);
    BlankingBits += 3u*8u*4u;
    u32 remain = surfaceWidth % numLanes;
    u32 PixelSteeringBits = remain ? ((numLanes - remain) * depth) : 0u;
    BlankingBits += PixelSteeringBits;
    u64 NumBlankingLinkClocks = ((u64)BlankingBits * PF) / (8ull * numLanes);
    u32 MinHBlank = (u32)(((NumBlankingLinkClocks * pixelClockHz) / minRate) / PF);
    MinHBlank += 12u;
    if (MinHBlank > rasterWidth - surfaceWidth) return false;
    if (surfaceWidth <= 60u) return false;

    s32 hbl = (s32)(((u64)(rasterWidth - surfaceWidth - MinHBlank) * minRate) / pixelClockHz);
    hbl -= 1; hbl -= 3;
    hbl -= (numLanes == 1u) ? 9 : (numLanes == 2u) ? 6 : 3;
    u32 hBlankSym = (hbl < 0) ? 0u : (u32)hbl;

    s32 vbl = 0;
    if (surfaceWidth >= 40u) {
        vbl = (s32)(((u64)(surfaceWidth - 40u) * minRate) / pixelClockHz) - 1;
        vbl -= (numLanes == 1u) ? 39 : (numLanes == 2u) ? 21 : 12;
    }
    u32 vBlankSym = (vbl < 0) ? 0u : (u32)vbl;

    *out_wm = waterMark; *out_hblank = hBlankSym; *out_vblank = vBlankSym;
    return true;
}

static void disp_push_dp_vsc_rgb8(dchan_t *core, u32 head) {
    dpush1(core, CA7D_HEAD_SET_VSC_SDP_CTRL(head),
           CA7D_VSC_SDP_CTRL_RGB8_FULL);
    dpush1(core, CA7D_HEAD_SET_VSC_SDP_HEADER(head),
           CA7D_VSC_SDP_HEADER_RGB8_FULL);
    for (u32 i = 0; i < 8u; i++)
        dpush1(core, CA7D_HEAD_SET_VSC_SDP_DATA0(head) + i * 4u,
               i == 4u ? CA7D_VSC_SDP_DATA4_RGB8_FULL : 0u);
}

/* nvDPPreSetMode() calls these two DPLib operations after all proposed head/SOR
 * methods have been assembled and immediately before nvDoIMPUpdateEvo() kicks
 * the core channel.  Even the normal RGB case sends the calls: stereo state is
 * cleared immediately and a zero-mask MSA override is cached for the upcoming
 * modeset.  Do the same for every affected SST connector, including the UEFI
 * primary, so a cold secondary is not relying on firmware-only MSA cache. */
static void disp_dp_pre_modeset_msa(nv_card_t *c, nv_rm_t *rm, u32 objcom,
                                    u32 display_id, const char *which) {
    dp_set_stereo_msa_t stereo; memset(&stereo, 0, sizeof stereo);
    stereo.displayId = display_id;
    bool stereo_ok = disp_ctrl(c, rm, objcom,
        NV0073_CTRL_CMD_DP_SET_STEREO_MSA_PROPERTIES,
        &stereo, sizeof stereo, "DP pre-modeset clear stereo MSA");

    dp_set_msa_t msa; memset(&msa, 0, sizeof msa);
    msa.displayId = display_id;
    msa.bCacheMsaOverrideForNextModeset = 1u;
    bool msa_ok = disp_ctrl(c, rm, objcom,
        NV0073_CTRL_CMD_DP_SET_MSA_PROPERTIES,
        &msa, sizeof msa, "DP pre-modeset cache MSA");
    kinfo("nv-disp", "  %s display %#x DP pre-modeset MSA: stereo %s cache %s",
          which, display_id, stereo_ok ? "OK" : "unsupported/refused",
          msa_ok ? "OK" : "unsupported/refused");
}

/* EvoSetMultiTileConfigCA + SetTileSize.  A logical window is not a physical
 * window number: WINDOW_SET_PHYSICAL is the mask assigned by the NVKMS resource
 * allocator.  The earlier identity mapping (window2->phywin2,
 * window4->phywin4) skipped Linux's allocator and is removed here. */
static void disp_push_multitile(dchan_t *core, u32 head, u32 window,
                                u32 hactive, u32 tiles_mask, u32 phywins_mask,
                                bool dsc, u32 dsc_slices) {
    dpush1(core, CA7D_HEAD_SET_TILE_MASK(head), tiles_mask);
    dpush1(core, CA7D_WINDOW_SET_PHYSICAL(window), phywins_mask);
    u32 num_tiles = disp_popcount(tiles_mask);
    if (!num_tiles) return;
    u32 start = 0u, tile_index = 0u;
    for (u32 tile = 0; tile < 8u; tile++) {
        if (!(tiles_mask & (1u << tile))) continue;
        u32 width;
        if (tile_index == num_tiles - 1u) {
            width = hactive - start;
        } else if (dsc && dsc_slices) {
            u32 slice_width = (hactive + dsc_slices - 1u) / dsc_slices;
            u32 slices_here = dsc_slices <= num_tiles ? 1u : dsc_slices / num_tiles;
            if (dsc_slices > num_tiles && tile_index < dsc_slices % num_tiles)
                slices_here++;
            width = slices_here * slice_width;
        } else {
            width = (hactive + num_tiles - 1u) / num_tiles;
        }
        dpush1(core, CA7D_TILE_SET_TILE_SIZE(tile),
               (start & 0x7fffu) | ((width & 0x7fffu) << 16));
        start += width;
        tile_index++;
    }
}

static void disp_push_hdmi_dsc(dchan_t *core, u32 head,
                               const hdmi_mode_plan_t *p);

static void disp_push_head_mode(dchan_t *core, u32 head, u32 window, u32 sor,
                                u32 sor_protocol, u32 display_id,
                                const edid_mode_t *m,
                                const dp_mode_plan_t *plan,
                                const hdmi_mode_plan_t *hdmi_plan) {
    u32 hactive = m->hactive, vactive = m->vactive;
    u32 htotal = m->htotal, vtotal = m->vtotal;
    u32 hs = hactive + m->hsync_off, he = hs + m->hsync_w;
    u32 vs = vactive + m->vsync_off, ve = vs + m->vsync_w;
    u32 leading = vtotal - vs, trailing = vs - vactive;
    if (leading < 2u) leading = 2u;

    /* ApplyProposedModeSetHwStateOneHeadPreUpdate() ordering from NVIDIA's
     * nvkm-modeset.c: LUT -> timings/DSC -> multi-tile -> VSC -> dithering ->
     * output resource -> connector attach -> viewport.  The UEFI head masked
     * the former arbitrary ordering; cold heads do not inherit those shadows. */
    if (g_olut_ok) {
        dpush1(core, CA7D_HEAD_SET_SURFACE_ADDRESS_HI_OLUT(head),
               (u32)(g_olut_fb >> 32));
        dpush1(core, CA7D_HEAD_SET_SURFACE_ADDRESS_LO_OLUT(head),
               ((u32)g_olut_fb & 0xFFFFFFF0u) |
               (CA7E_LO_ISO_TARGET_PHYSICAL_NVM << 2) | 1u);
        dpush1(core, CA7D_HEAD_SET_OLUT_CONTROL(head), CA7D_OLUT_CONTROL_VAL);
        dpush1(core, CA7D_HEAD_SET_OLUT_FP_NORM_SCALE(head), 0xFFFFFFFFu);
    }

    dpush1(core, CA7D_HEAD_SET_OVERSCAN_COLOR(head), 0u);
    dpush1(core, CA7D_HEAD_SET_RASTER_SIZE(head),
           (htotal & 0xFFFFu) | (vtotal << 16));
    dpush1(core, CA7D_HEAD_SET_RASTER_SYNC_END(head),
           ((he - hs - 1u) & 0x7FFFu) | (((ve - vs - 1u) & 0x7FFFu) << 16));
    dpush1(core, CA7D_HEAD_SET_RASTER_BLANK_END(head),
           ((htotal - hs - 1u) & 0x7FFFu) | (((vtotal - vs - 1u) & 0x7FFFu) << 16));
    dpush1(core, CA7D_HEAD_SET_RASTER_BLANK_START(head),
           ((htotal - hs - 1u + hactive) & 0x7FFFu) |
           (((vtotal - vs - 1u + vactive) & 0x7FFFu) << 16));
    dpush1(core, CA7D_HEAD_SET_MIN_FRAME_IDLE(head),
           (leading & 0x7FFFu) | ((trailing & 0x7FFFu) << 16));
    dpush1(core, CA7D_HEAD_SET_CONTROL(head), CA7D_STRUCTURE_PROGRESSIVE);
    /* EvoSetHeadControlC9 immediately refreshes the global flip-lock control.
     * Zero is the exact unlocked value, but the method itself is part of the
     * first cold-head programming sequence. */
    dpush1(core, CA7D_SET_CONTROL, 0u);
    dpush1(core, CA7D_HEAD_SET_LOCK_CHAIN(head), 0u);
    u64 hz = (u64)m->pixel_clock_khz * 1000ull;
    dpush1(core, CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(head),
           (u32)(hz & 0x7FFFFFFFull));
    dpush1(core, CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI(head),
           (u32)((hz >> 31) & 0xFu));
    dpush1(core, CA7D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION(head), 0u);
    dpush1(core, CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(head),
           (u32)(hz & 0x7FFFFFFFull));
    dpush1(core, CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI_MAX(head),
           (u32)((hz >> 31) & 0xFu));
    dpush1(core, CA7D_HEAD_SET_FRAME_PACKED_VACTIVE_COLOR(head), 0u);
    dpush1(core, CA7D_HEAD_SET_HDMI_CTRL(head), 0u);
    dpush1(core, CA7D_HEAD_SET_RASTER_HBLANK_DELAY(head), 0u);
    dpush1(core, CA7D_HEAD_SET_SW_SPARE_A(head), 0u);
    if (hdmi_plan && hdmi_plan->dsc) {
        disp_push_hdmi_dsc(core, head, hdmi_plan);
    } else if (plan && plan->enabled) {
        u32 ctl = CA7D_DSC_CONTROL_ENABLE |
                  CA7D_DSC_CONTROL_FULL_ICH_PRECISION |
                  CA7D_DSC_CONTROL_FORCE_ICH_RESET |
                  (2u << CA7D_DSC_CONTROL_FLATNESS_SHIFT);
        dpush1(core, CA7D_HEAD_SET_DSC_CONTROL(head), ctl);
        dpush1(core, CA7D_HEAD_SET_DSC_PPS_CONTROL(head),
               CA7D_DSC_PPS_ENABLE | CA7D_DSC_PPS_LOCATION_VSYNC |
               CA7D_DSC_PPS_SIZE_128_BYTES);
        for (u32 i = 0; i < DSC_MAX_PPS_SIZE_DWORD; i++)
            dpush1(core, CA7D_HEAD_SET_DSC_PPS_DATA0(head) + i * 4u,
                   plan->pps[i]);
        dpush1(core, CA7D_HEAD_SET_DSC_PPS_HEAD(head), 0x007f1000u);
    } else {
        dpush1(core, CA7D_HEAD_SET_DSC_CONTROL(head), 0u);
        dpush1(core, CA7D_HEAD_SET_DSC_PPS_CONTROL(head), 0u);
    }

    /* EvoSetMultiTileConfigCA follows timing/DSC setup.  InitWindowMappingCA
     * cleared the cold head/window defaults earlier in this same proposal. */
    disp_push_multitile(core, head, window, hactive,
                        g_imp_tile_mask[head], g_imp_phywin_mask[head],
                        g_imp_dsc_slices[head] != 0u,
                        g_imp_dsc_slices[head]);

    if (sor_protocol == OR_PROTOCOL_SOR_DP_A ||
        sor_protocol == OR_PROTOCOL_SOR_DP_B)
        disp_push_dp_vsc_rgb8(core, head);
    else
        dpush1(core, CA7D_HEAD_SET_VSC_SDP_CTRL(head), 0u);

    dpush1(core, CA7D_HEAD_SET_DITHER_CONTROL(head), 0u);
    dpush1(core, CA7D_HEAD_SET_PROCAMP(head), CA7D_PROCAMP_RGB_VESA);
    u32 out_res = 1u | (CA7D_PIXEL_DEPTH_BPP_24_444 << 4) |
                  CA7D_OUT_RES_EXT_PACKET_WIN_NONE;
    if ((m->flags & 0x18u) == 0x18u) {
        if (!(m->flags & 0x02u)) out_res |= 1u << 2;
        if (!(m->flags & 0x04u)) out_res |= 1u << 3;
    }
    dpush1(core, CA7D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(head), out_res);

    dpush1(core, CA7D_SOR_SET_CONTROL(sor),
           (1u << head) | (sor_protocol << CA7D_SOR_PROTOCOL_SHIFT));
    dpush1(core, CA7D_HEAD_SET_DISPLAY_ID(head, 0), display_id);

    dpush1(core, CA7D_HEAD_SET_VIEWPORT_POINT_IN(head), 0u);
    dpush1(core, CA7D_HEAD_SET_VIEWPORT_SIZE_IN(head),
           (hactive & 0x7FFFu) | (vactive << 16));
    dpush1(core, CA7D_HEAD_SET_VIEWPORT_SIZE_OUT(head),
           (hactive & 0x7FFFu) | (vactive << 16));
    dpush1(core, CA7D_HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(head), 0u);
    dpush1(core, CA7D_HEAD_SET_CONTROL_OUTPUT_SCALER(head),
           CA7D_OUTPUT_SCALER_TAPS_2);
    dpush1(core, CA7D_HEAD_SET_MAX_OUTPUT_SCALE_FACTOR(head),
           CA7D_SCALE_FACTOR_1X | (CA7D_SCALE_FACTOR_1X << 16));
}

static void disp_push_hdmi_dsc(dchan_t *core, u32 head,
                               const hdmi_mode_plan_t *p) {
    if (!p || !p->dsc) return;
    u32 ctl = CA7D_DSC_CONTROL_ENABLE | CA7D_DSC_CONTROL_FULL_ICH_PRECISION |
              (1u << 3) | /* AUTO_RESET for HDMI FRL */
              (2u << CA7D_DSC_CONTROL_FLATNESS_SHIFT);
    dpush1(core, CA7D_HEAD_SET_DSC_CONTROL(head), ctl);
    dpush1(core, CA7D_HEAD_SET_DSC_PPS_CONTROL(head),
           CA7D_DSC_PPS_ENABLE | CA7D_DSC_PPS_SIZE_HDMI_CVTEM);
    for (u32 i = 0; i < DSC_MAX_PPS_SIZE_DWORD; i++)
        dpush1(core, CA7D_HEAD_SET_DSC_PPS_DATA0(head) + i * 4u, p->pps[i]);
    dpush1(core, CA7D_HEAD_SET_DSC_PPS_HEAD(head), 0x7fu);
    dpush1(core, CA7D_HEAD_SET_HDMI_DSC_HCACTIVE(head),
           (p->hactive_bytes & 0xffffu) | (p->hactive_tribytes << 16));
    dpush1(core, CA7D_HEAD_SET_HDMI_DSC_HCBLANK(head), p->hblank_tribytes);
}

static void disp_push_window_surface(dchan_t *win, u32 window, u32 fmt,
                                     u64 pitch, u64 fb,
                                     const edid_mode_t *m) {
    u32 size = ((u32)m->hactive & 0xFFFFu) | ((u32)m->vactive << 16);
    dpush1(win, CA7E_SET_SIZE, size);
    dpush1(win, CA7E_SET_STORAGE, 0u);
    dpush1(win, CA7E_SET_PARAMS, fmt);
    dpush1(win, CA7E_SET_PLANAR_STORAGE(0), (u32)((pitch >> 6) & 0x1FFFu));
    dpush1(win, CA7E_SET_SURFACE_ADDRESS_HI_ISO(0), (u32)(fb >> 32));
    dpush1(win, CA7E_SET_SURFACE_ADDRESS_LO_ISO(0),
           ((u32)fb & 0xFFFFFFF0u) |
           (CA7E_LO_ISO_TARGET_PHYSICAL_NVM << 2) |
           (CA7E_LO_ISO_KIND_PITCH << 1) | CA7E_LO_ISO_ENABLE);
    dpush1(win, CA7E_SET_POINT_IN(0), 0u);
    dpush1(win, CA7E_SET_SIZE_IN, size);
    dpush1(win, CA7E_SET_SIZE_OUT, size);
    dpush1(win, CA7E_SET_PRESENT_CONTROL, 0u);
    /* NVKMS decouples flips when first assigning window ownership.  The core
     * modeset has already promoted, so this window update is intentionally not
     * interlocked with core or sibling windows. */
    dpush1(win, CA7E_SET_INTERLOCK_FLAGS, 0u);
    dpush1(win, CA7E_SET_WINDOW_INTERLOCK_FLAGS, 0u);
    dpush1(win, CA7E_SW_SET_MCLK_SWITCH, 0u);
    dpush1(win, CA7E_UPDATE, 1u);
    dpush1(win, CA7E_SW_SET_MCLK_SWITCH, 1u);
}

static bool disp_reconfigure_uncompressed_dp_stream(nv_card_t *c, nv_rm_t *rm,
                                                     u32 objcom, u32 head,
                                                     u32 sor, u32 protocol,
                                                     u32 link_bw, u32 lanes,
                                                     bool enhanced,
                                                     const edid_mode_t *m,
                                                     const char *what) {
    dp_config_stream_t cs; memset(&cs, 0, sizeof cs);
    u32 wm = 0u, hbs = 0u, vbs = 0u;
    if (!disp_dp_watermark_sst(link_bw, lanes, m->pixel_clock_khz,
                               m->hactive, m->htotal, enhanced,
                               &wm, &hbs, &vbs))
        return false;
    cs.head = head;
    cs.sorIndex = sor;
    cs.dpLink = protocol == OR_PROTOCOL_SOR_DP_B ? 1u : 0u;
    cs.bEnableOverride = 1u;
    cs.hBlankSym = hbs;
    cs.vBlankSym = vbs;
    cs.SST.bEnhancedFraming = enhanced ? 1u : 0u;
    cs.SST.tuSize = 64u;
    cs.SST.waterMark = wm;
    return disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DP_CONFIG_STREAM,
                     &cs, sizeof cs, what);
}

/* Add one non-GOP DP or HDMI-FRL sink on its own head/window while keeping all
 * previously committed outputs in the DISPLAY_CHANGE mask. */
static bool disp_prepare_secondary(nv_card_t *c, nv_rm_t *rm, u32 objcom,
                                   disp_sink_t *sink, u32 head, u32 window,
                                   u64 pitch, u64 surface_bytes,
                                   u32 *used_sor_mask,
                                   disp_secondary_plan_t *out) {
    if (!sink->valid || head >= 4u || window >= 8u) return false;
    if (g_relight.num_heads && head >= g_relight.num_heads) return false;
    if (g_relight.windows_present && !(g_relight.windows_present & (1u << window)))
        return false;

    kinfo("nv-disp", "secondary: display %#x -> head%u window%u", sink->display_id,
          head, window);

    bool ok = false, is_hdmi = false;
    edid_mode_t mode; memset(&mode, 0, sizeof mode);
    dp_mode_plan_t plan; memset(&plan, 0, sizeof plan);
    hdmi_mode_plan_t hdmi_plan; memset(&hdmi_plan, 0, sizeof hdmi_plan);
    memset(out, 0, sizeof *out);
    if (!dchan_alloc(c, rm, &out->win, GB202_DISP_WINDOW_CHANNEL_DMA, window,
                     WINDOW_USER_BASE(window), "secondary window"))
        goto secondary_done;

    or_get_info_t ori; memset(&ori, 0, sizeof ori);
    ori.displayId = sink->display_id;
    if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO,
                   &ori, sizeof ori, "secondary OR get info"))
        goto secondary_done;
    bool is_dp = ori.protocol == OR_PROTOCOL_SOR_DP_A ||
                 ori.protocol == OR_PROTOCOL_SOR_DP_B;
    is_hdmi = ori.protocol == OR_PROTOCOL_SOR_TMDS_A ||
              ori.protocol == OR_PROTOCOL_SOR_TMDS_B ||
              ori.protocol == OR_PROTOCOL_SOR_TMDS_DUAL;
    if (!is_dp && !is_hdmi) {
        kwarn("nv-disp", "secondary display %#x uses unsupported protocol %u",
              sink->display_id, ori.protocol);
        goto secondary_done;
    }
    u32 protocol = is_hdmi ? ori.protocol :
                   (ori.protocol == OR_PROTOCOL_SOR_DP_B ?
                    OR_PROTOCOL_SOR_DP_B : OR_PROTOCOL_SOR_DP_A);

    dfp_assign_sor_t asor; memset(&asor, 0, sizeof asor);
    asor.displayId = sink->display_id;
    /* NVKMS AssignSor excludes every SOR already committed to another connector.
     * Without this, RM is allowed to reuse the live GOP SOR for the new display. */
    asor.sorExcludeMask = (u8)*used_sor_mask;
    if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DFP_ASSIGN_SOR,
                   &asor, sizeof asor, "secondary assign SOR"))
        goto secondary_done;
    u32 sor = 0xFFFFFFFFu;
    for (u32 i = 0; i < 4u; i++)
        if (asor.sorAssignListWithTag[i].displayMask & sink->display_id) {
            sor = i; break;
        }
    if (sor == 0xFFFFFFFFu) goto secondary_done;

    /* An inactive connector has no old head to shut down.  NVKMS assigns its
     * SOR, trains the transport, and includes the attach in the one proposed
     * modeset update.  Detaching/releasing an unowned secondary here created an
     * extra state transition that the reference driver never performs. */
    *used_sor_mask |= 1u << sor;
    sink->attach_attempted = true;
    sink->assigned_head = head;
    sink->assigned_sor = sor;
    kinfo("nv-disp", "secondary display %#x: RM route SOR%u (excluded prior SORs %#x)",
          sink->display_id, sor, asor.sorExcludeMask);

    dp_get_caps_t caps; memset(&caps, 0, sizeof caps);
    caps.sorIndex = sor;
    u32 card_bw = 0;
    if (disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DP_GET_CAPS,
                  &caps, sizeof caps, "secondary DP get caps") && caps.maxLinkRate)
        card_bw = dp_link_bw_code(caps.maxLinkRate);

    if (is_hdmi) {
        const edid_hdmi_forum_t *hf = &sink->edid.hdmi_forum;
        /* NVIDIA requires this control before every HDMI-capable modeset.  It
         * tells RM this is HDMI rather than DVI and enables the HDMI resource
         * path; a trained FRL/TMDS link without it is not a complete signal. */
        hdmi_enable_rm_t he; memset(&he, 0, sizeof he);
        he.displayId = sink->display_id; he.enable = 1u;
        if (!disp_ctrl(c, rm, objcom,
                       NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_ENABLE,
                       &he, sizeof he, "HDMI pre-modeset enable"))
            goto secondary_done;

        hdmi_sink_caps_rm_t sc; memset(&sc, 0, sizeof sc);
        sc.displayId = sink->display_id;
        sc.caps = ((u32)(hf->max_tmds_clock_khz > 340000u) << 0) |
                  ((u32)hf->lte_340_scramble << 1) | ((u32)hf->scdc_present << 2) |
                  ((u32)hf->max_frl_rate << 3) | ((u32)hf->dsc_1p2 << 6) |
                  ((u32)hf->dsc_max_frl_rate << 7);
        if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_SINK_CAPS,
                       &sc, sizeof sc, "HDMI set sink caps"))
            goto secondary_done;

        /* Build both legal transports and choose the best real EDID pair.
         * HDMI 1.x/2.0 sinks are not rejected merely because FRL is absent;
         * HDMI 2.1 uses FRL (and DSC if necessary) only when that produces a
         * strictly better resolution/refresh pair than TMDS. */
        u32 tmds_cap = hf->scdc_present ? 594000u : 340000u;
        if (hf->max_tmds_clock_khz && hf->max_tmds_clock_khz < tmds_cap)
            tmds_cap = hf->max_tmds_clock_khz;
        edid_mode_t tmds_mode; memset(&tmds_mode, 0, sizeof tmds_mode);
        bool have_tmds = disp_pick_best_tmds_mode(&sink->edid, tmds_cap,
                                                  pitch, surface_bytes,
                                                  &tmds_mode);

        hdmi_gpu_caps_rm_t gc; memset(&gc, 0, sizeof gc);
        edid_mode_t frl_mode; memset(&frl_mode, 0, sizeof frl_mode);
        hdmi_mode_plan_t frl_plan; memset(&frl_plan, 0, sizeof frl_plan);
        bool have_frl = false;
        u32 max_frl = 0u, dsc_frl = 0u;
        if (hf->present && hf->scdc_present && hf->max_frl_rate &&
            disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_GET_HDMI_GPU_CAPS,
                      &gc, sizeof gc, "HDMI get GPU FRL caps")) {
            u32 gpu_frl = gc.caps & 7u;
            max_frl = hf->max_frl_rate < gpu_frl ? hf->max_frl_rate : gpu_frl;
            dsc_frl = hf->dsc_max_frl_rate < max_frl ?
                      hf->dsc_max_frl_rate : max_frl;
            if (max_frl)
                have_frl = disp_pick_best_hdmi_mode(c, rm, objcom,
                    sink->display_id, &sink->edid, pitch, surface_bytes,
                    &caps, max_frl, dsc_frl, &frl_mode, &frl_plan);
        }
        bool use_frl = have_frl && (!have_tmds ||
            (u64)frl_mode.hactive * frl_mode.vactive >
                (u64)tmds_mode.hactive * tmds_mode.vactive ||
            ((u64)frl_mode.hactive * frl_mode.vactive ==
                 (u64)tmds_mode.hactive * tmds_mode.vactive &&
             frl_mode.refresh_mhz > tmds_mode.refresh_mhz));
        if (!use_frl) {
            if (!have_tmds) goto secondary_done;
            mode = tmds_mode;
            protocol = ori.protocol;
            sink->best_mode = mode;
            kinfo("nv-disp", "HDMI display %#x: %ux%u @ %u.%03u Hz, TMDS %u kHz",
                  sink->display_id, mode.hactive, mode.vactive,
                  mode.refresh_mhz / 1000u, mode.refresh_mhz % 1000u,
                  mode.pixel_clock_khz);
            goto transport_ready;
        }
        mode = frl_mode; hdmi_plan = frl_plan;
        protocol = OR_PROTOCOL_SOR_HDMI_FRL;
        sink->best_mode = mode;
        hdmi_frl_config_t fc; memset(&fc, 0, sizeof fc);
        fc.displayId = sink->display_id; fc.data = hdmi_plan.frl_rate;
        if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_FRL_CONFIG,
                       &fc, sizeof fc, "HDMI FRL link training") ||
            !(fc.data & 7u))
            goto secondary_done;
        u32 trained_frl = fc.data & 7u;
        if (trained_frl < hdmi_plan.frl_rate) {
            /* The source and sink may both advertise FRL6 while the actual cable
             * only trains lower.  Re-run the complete uncompressed/DSC capacity
             * search at the empirically trained cable rate; never keep a mode
             * chosen against a bandwidth the cable failed to establish. */
            hdmi_mode_plan_t fallback_plan; memset(&fallback_plan, 0, sizeof fallback_plan);
            edid_mode_t fallback_mode; memset(&fallback_mode, 0, sizeof fallback_mode);
            u32 fallback_dsc_frl = dsc_frl < trained_frl ? dsc_frl : trained_frl;
            if (!disp_pick_best_hdmi_mode(c, rm, objcom, sink->display_id,
                    &sink->edid, pitch, surface_bytes, &caps, trained_frl,
                    fallback_dsc_frl, &fallback_mode, &fallback_plan))
                goto secondary_done;
            mode = fallback_mode; hdmi_plan = fallback_plan;
            sink->best_mode = mode;
            if (hdmi_plan.frl_rate != trained_frl) {
                memset(&fc, 0, sizeof fc);
                fc.displayId = sink->display_id;
                fc.data = hdmi_plan.frl_rate;
                if (!disp_ctrl(c, rm, objcom,
                        NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_FRL_CONFIG,
                        &fc, sizeof fc, "HDMI FRL fallback training") ||
                    (fc.data & 7u) != hdmi_plan.frl_rate)
                    goto secondary_done;
                trained_frl = fc.data & 7u;
            }
        }
        hdmi_plan.frl_rate = trained_frl;
        kinfo("nv-disp", "HDMI display %#x: %ux%u @ %u.%03u Hz, FRL%u %s",
              sink->display_id, mode.hactive, mode.vactive,
              mode.refresh_mhz / 1000u, mode.refresh_mhz % 1000u,
              hdmi_plan.frl_rate, hdmi_plan.dsc ? "DSC" : "uncompressed");
        goto transport_ready;
    }

    nv_dpcd_t dpcd; memset(&dpcd, 0, sizeof dpcd);
    bool have_dpcd = nv_dp_read_dpcd(c, rm, objcom, sink->display_id, &dpcd) && dpcd.valid;
    u32 sink_bw = 0;
    if (have_dpcd) switch (dpcd.max_link_rate_code) {
        case DP_LINK_BW_1_62: case 0x08u: case 0x09u: case DP_LINK_BW_2_70:
        case 0x0cu: case 0x10u: case DP_LINK_BW_5_40: case DP_LINK_BW_8_10:
            sink_bw = dpcd.max_link_rate_code; break;
        default: break;
    }
    u32 link_bw = card_bw && sink_bw ? (card_bw < sink_bw ? card_bw : sink_bw) :
                  card_bw ? card_bw : sink_bw ? sink_bw : DP_LINK_BW_1_62;
    u32 lanes = have_dpcd ? (dpcd.max_lanes >= 4u ? 4u :
                              dpcd.max_lanes >= 2u ? 2u : 1u) : 4u;
    bool enhanced = have_dpcd && dpcd.enhanced_framing;
    dp_sink_dsc_t sink_dsc; memset(&sink_dsc, 0, sizeof sink_dsc);
    if (have_dpcd && dpcd.rev >= 0x14u)
        disp_read_dp_dsc_caps(c, rm, objcom, sink->display_id, &sink_dsc);
    if (!disp_pick_best_dp_mode(c, rm, objcom, sink->display_id, head,
                                &sink->edid, link_bw, lanes, enhanced,
                                pitch, surface_bytes, &caps, &sink_dsc,
                                &mode, &plan)) {
        kwarn("nv-disp", "secondary display %#x: no exact EDID mode fits negotiated DP+DSC/scanout",
              sink->display_id);
        goto secondary_done;
    }
    sink->best_mode = mode;
    kinfo("nv-disp", "secondary display %#x: EDID-selected %ux%u @ %u.%03u Hz, SOR%u, %u lanes bw %#x, %s",
          sink->display_id, mode.hactive, mode.vactive,
          mode.refresh_mhz / 1000u, mode.refresh_mhz % 1000u, sor, lanes, link_bw,
          plan.enabled ? "DSC+FEC" : "uncompressed");

    nv_dp_write_dpcd(c, rm, objcom, sink->display_id, 0x600u, 0x1u);
    u32 dp_cmd = DP_CMD_SET_LANE_COUNT | DP_CMD_SET_LINK_BW |
                 (enhanced ? DP_CMD_SET_ENHANCED_FRAMING : 0u) |
                 (plan.enabled ? DP_CMD_ENABLE_FEC : 0u);
    static const u32 rates[] = { DP_LINK_BW_8_10, DP_LINK_BW_5_40,
                                 DP_LINK_BW_2_70, DP_LINK_BW_1_62 };
    bool trained = false;
    for (u32 r = 0; r < sizeof rates / sizeof rates[0] && !trained; r++) {
        if (rates[r] > link_bw) continue;
        for (u32 attempt = 0; attempt < 8u; attempt++) {
            dp_ctrl_t dp; memset(&dp, 0, sizeof dp);
            dp.displayId = sink->display_id;
            dp.cmd = dp_cmd;
            dp.data = (lanes << DP_DATA_LANE_COUNT_SHIFT) |
                      (rates[r] << DP_DATA_LINK_BW_SHIFT);
            u32 got = 0;
            bool rpc_ok = nv_rm_control(c, rm, objcom, NV0073_CTRL_CMD_DP_CTRL,
                                        &dp, sizeof dp, &dp, sizeof dp, &got);
            kinfo("nv-disp", "  secondary DP train bw %#x attempt %u: rpc %d err %#x retry %u",
                  rates[r], attempt + 1u, rpc_ok, dp.err, dp.retryTimeMs);
            if (rpc_ok && !dp.err && !dp.retryTimeMs) {
                trained = true; link_bw = rates[r]; break;
            }
            if (!dp.retryTimeMs) break;
            u64 dl = g_uptime_ms + dp.retryTimeMs;
            while (g_uptime_ms < dl) timer_udelay(500);
        }
    }
    if (!trained) goto secondary_done;

    /* A lower fallback rate may no longer carry the initially selected mode. */
    bool trained_with_fec = plan.enabled;
    if (!disp_pick_best_dp_mode(c, rm, objcom, sink->display_id, head,
                                &sink->edid, link_bw, lanes, enhanced,
                                pitch, surface_bytes, &caps, &sink_dsc,
                                &mode, &plan))
        goto secondary_done;
    sink->best_mode = mode;
    if (plan.enabled && !trained_with_fec) {
        dp_ctrl_t dp; memset(&dp, 0, sizeof dp);
        dp.displayId = sink->display_id;
        dp.cmd = DP_CMD_SET_LANE_COUNT | DP_CMD_SET_LINK_BW |
                 (enhanced ? DP_CMD_SET_ENHANCED_FRAMING : 0u) |
                 DP_CMD_ENABLE_FEC;
        dp.data = (lanes << DP_DATA_LANE_COUNT_SHIFT) |
                  (link_bw << DP_DATA_LINK_BW_SHIFT);
        u32 got = 0;
        if (!nv_rm_control(c, rm, objcom, NV0073_CTRL_CMD_DP_CTRL,
                           &dp, sizeof dp, &dp, sizeof dp, &got) || dp.err)
            goto secondary_done;
        trained_with_fec = true;
    }
    if (plan.enabled) {
        dp_configure_fec_t fec; memset(&fec, 0, sizeof fec);
        fec.displayId = sink->display_id; fec.bEnableFec = 1u;
        if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DP_CONFIGURE_FEC,
                       &fec, sizeof fec, "secondary DP enable GPU FEC") ||
            !nv_dp_write_dpcd(c, rm, objcom, sink->display_id, 0x160u, 1u))
            goto secondary_done;
    }
    dp_config_stream_t cs; memset(&cs, 0, sizeof cs);
    cs.head = head; cs.sorIndex = sor;
    cs.dpLink = (protocol == OR_PROTOCOL_SOR_DP_B) ? 1u : 0u;
    cs.SST.bEnhancedFraming = enhanced ? 1u : 0u;
    cs.SST.tuSize = plan.enabled ? plan.tu : 64u;
    u32 wm = 0, hbs = 0, vbs = 0;
    if (plan.enabled) {
        wm = plan.wm; hbs = plan.hblank_sym; vbs = plan.vblank_sym;
    } else if (!disp_dp_watermark_sst(link_bw, lanes, mode.pixel_clock_khz,
                                      mode.hactive, mode.htotal, enhanced,
                                      &wm, &hbs, &vbs)) {
            goto secondary_done;
    }
    cs.bEnableOverride = 1u; cs.hBlankSym = hbs; cs.vBlankSym = vbs;
    cs.SST.waterMark = wm;
    if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DP_CONFIG_STREAM,
                   &cs, sizeof cs, "secondary DP config stream"))
        goto secondary_done;

transport_ready:
    out->ready = true;
    out->is_hdmi = is_hdmi;
    out->sink = sink;
    out->head = head;
    out->window = window;
    out->sor = sor;
    out->protocol = protocol;
    out->link_lanes = is_hdmi ? 0u : lanes;
    out->link_bw = is_hdmi ? 0u : link_bw;
    out->link_enhanced = is_hdmi ? false : enhanced;
    out->mode = mode;
    out->dp_plan = plan;
    out->hdmi_plan = hdmi_plan;
    ok = true;

secondary_done:
    return ok;
}

void nv_disp_relight_get_diag(nv_disp_relight_diag_t *out) { *out = g_relight; }

/* ------------------------------------------------------------- the light-up */
void nv_disp_lightup(nv_card_t *c, nv_rm_t *rm) {
    memset(&g_relight, 0, sizeof g_relight);
    g_relight.ran = true;
    g_relight.stopped_at = "entered";
    bool modeset_change_open = false;
    u32 modeset_change_mask = 0u;
    bool only_one_output = false;
    u32 display_id = 0u;
    disp_secondary_plan_t secondary[3];
    u32 secondary_count = 0u;
    memset(secondary, 0, sizeof secondary);
    kinfo("nv-disp", "DisplayPort re-light starting (displaytest) - UPDATE only "
                     "on a trained link");

    /* Do not trust the loader's one-monitor EDID as the machine topology.  It is
     * only a fallback until the dedicated NV0073 object can query every connected
     * NVIDIA output below. */
    edid_info_t *ed = &g_boot_edid; memset(ed, 0, sizeof *ed);
    edid_mode_t selected_mode; memset(&selected_mode, 0, sizeof selected_mode);
    dp_mode_plan_t primary_plan; memset(&primary_plan, 0, sizeof primary_plan);
    const edid_mode_t *m = &selected_mode;
    if (g_boot.display.present && g_boot.display.edid_len &&
        edid_parse(g_boot.display.edid, g_boot.display.edid_len, ed) && ed->valid)
        selected_mode = ed->native;
    u32 htotal = selected_mode.htotal, vtotal = selected_mode.vtotal;
    u32 hactive = g_boot.fb.width, vactive = g_boot.fb.height;
    u32 hsync_start = 0, hsync_end = 0, vsync_start = 0, vsync_end = 0;

    /* Framebuffer: reuse the live boot framebuffer.  Require 32bpp and one of the
     * two 8-8-8 layouts, else the FORMAT would be a guess. */
    if (g_boot.fb.bpp != 32 || !g_boot.fb.base) {
        kwarn("nv-disp", "boot framebuffer is not 32bpp - aborting"); return;
    }
    u32 fmt;
    if (g_boot.fb.red_shift == 16 && g_boot.fb.green_shift == 8 && g_boot.fb.blue_shift == 0)
        fmt = CA7E_FORMAT_X8R8G8B8;     /* BGRX in memory */
    else if (g_boot.fb.red_shift == 0 && g_boot.fb.green_shift == 8 && g_boot.fb.blue_shift == 16)
        fmt = CA7E_FORMAT_A8R8G8B8;
    else { kwarn("nv-disp", "unrecognised fb pixel order - aborting"); return; }
    if (g_boot.fb.pitch & 63u) {
        kwarn("nv-disp", "fb pitch %u not a multiple of 64 - aborting", g_boot.fb.pitch);
        return;
    }

    /* Reuse the scanout surface pre-allocated by nv_chan_open_engines after all
     * execution contexts.  The earlier 23.6 MiB late request fragmented out,
     * but the tight maximum-visible scanout is now only 14.1 MiB.  Returned
     * hardware proved that reserving even that compact surface before the real
     * GR channels starves their MAIN contexts, so execution contexts win the
     * constrained heap and display follows with PRIMARY then IMAGE fallback. */
    u32 red_px = (0xFFu << g_boot.fb.red_shift);
    u64 surf_pitch = (((u64)g_boot.fb.width * 4ull) + 63ull) & ~63ull;
    u64 surf_fb = g_scanout_fb;
    u64 surf_bytes = g_scanout_bytes ? g_scanout_bytes :
                     (surf_pitch * (u64)g_boot.fb.height);
    bool have_surf = g_scanout_ok;
    if (!have_surf) {
        /* Prealloc missed (nodisplay set late, or VRAM gone) - try once here as a
         * fallback so the modeset still has a chance. */
        g_relight.stopped_at = "allocating RGB-test VRAM scanout surface (fallback)";
        have_surf = nv_vram_alloc_scanout(c, rm, H_DISP_SCANOUT, surf_bytes, &surf_fb);
        if (!have_surf) {
            g_relight.rgb_alloc_status = nv_last_alloc_status;
        } else {
            g_scanout_ok = true;
            g_scanout_fb = surf_fb;
            g_scanout_bytes = surf_bytes;
            g_scanout_handle = H_DISP_SCANOUT;
        }
    }
    kinfo("nv-disp", "  RGB scanout surface: %s %llu B @ FB %#llx (prealloc %d, status %#x)",
          have_surf ? "OK" : "FAILED", (unsigned long long)surf_bytes,
          (unsigned long long)surf_fb, (int)g_scanout_ok,
          have_surf ? 0u : g_relight.rgb_alloc_status);
    if (!have_surf) {
        /* Never reinterpret the firmware's CPU/BAR address as a local FB
         * offset.  The 05:38 boot did that with 0xa000000000, immediately
         * removing signal from the only live monitor.  Without verified local
         * VRAM there is no safe window address and no secondary test surface;
         * leave the firmware-owned configuration untouched. */
        g_relight.stopped_at = "no verified VRAM-local scanout; preserving firmware display";
        kerr("nv-disp", "no verified VRAM-local scanout; refusing modeset instead of programming the GOP CPU address as PHYSICAL_NVM");
        return;
    }

    /* -------- object tree in nouveau's EXACT order (r535_disp_oneinit,
     * disp.c:1503, then r535_disp_init, disp.c:1455) --------
     *   RAMIN -> WRITE_INST_MEM (internal subdev) -> dedicated disp
     *   client+device -> NV04_DISPLAY_COMMON objcom under that device ->
     *   get_static_info (internal subdev) -> DP_SET_MANUAL_DISPLAYPORT (objcom)
     *   -> 0xca70 disp-engine root under that device.
     * Our old code never allocated the objcom object (it fired NV0073 controls
     * at a bare handle with no object), and put the disp objects under the
     * SHARED RM_DEVICE - the first HW boot died at the very first object alloc. */

    /* 1. RAMIN (display instance block) in VRAM.  nouveau: nvkm_gpuobj_new on the
     *    base device, target VRAM (disp.c:1513-1518).  See the TODO in the file
     *    header on the parent if a finding moves it under the disp client. */
    g_relight.stopped_at = "allocating RAMIN (display instance block)";
    u64 ramin_fb = 0;
    if (!nv_vram_alloc(c, rm, H_DISP_RAMIN, 0x10000, &ramin_fb)) {
        g_relight.disp_root_status = nv_last_alloc_status;  /* reuse the field for the RAMIN refusal code */
        kerr("nv-disp", "no VRAM for the display instance block (status %#x)", nv_last_alloc_status);
        return;
    }
    /* 2. Register it with GSP-RM (WRITE_INST_MEM) on the internal privileged
     *    subdevice BEFORE the disp objects, exactly as disp.c:1520-1531
     *    (nouveau targets gsp->internal.device.subdevice; RM_SUBDEVICE routes
     *    through nv_rm_control_internal / RPC 65). */
    disp_write_inst_mem_t wim; memset(&wim, 0, sizeof wim);
    wim.instMemPhysAddr = ramin_fb;
    wim.instMemSize = 0x10000;
    wim.instMemAddrSpace = DISP_ADDR_FBMEM;
    wim.instMemCpuCacheAttr = DISP_MEM_WRITECOMBINED;
    g_relight.stopped_at = "WRITE_INST_MEM (register RAMIN)";
    g_relight.inst_mem_ok = disp_ctrl(c, rm, RM_SUBDEVICE,
              NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM,
              &wim, sizeof wim, "write inst mem");

    /* 3. Build the DEDICATED disp client+device+subdevice (disp.c:1536,
     *    nvkm_gsp_client_device_ctor).  The disp objcom + engine root parent off
     *    the returned DISP DEVICE handle, NOT RM_DEVICE. */
    g_relight.stopped_at = "disp client+device ctor";
    u32 disp_client = 0, disp_device = 0, disp_subdevice = 0;
    if (!nv_rm_disp_client_ctor(c, rm, &disp_client, &disp_device, &disp_subdevice)) {
        kerr("nv-disp", "dedicated disp client+device ctor failed - aborting");
        return;
    }
    kinfo("nv-disp", "  disp client %#x device %#x subdevice %#x",
          disp_client, disp_device, disp_subdevice);

    /* The entire disp object tree (objcom, 0xca70 root, core/window channels) and
     * every NV0073 control lives under the DEDICATED disp client, exactly like
     * nouveau's disp->rm.client.  nv_rm_alloc/nv_rm_control stamp the RPC hClient
     * from rm->client, and the ctor restored rm->client to the MAIN client on
     * return - so without this retarget the objcom alloc's parent (disp_device,
     * which lives under the disp client) is looked up in the MAIN client's tree
     * and GSP-RM answers OBJECT_NOT_FOUND (0x57), which is exactly what the first
     * boot of this rewrite showed.  Scope rm->client to the disp client for the
     * whole sequence and restore it at the single `done:` exit (the re-light runs
     * before compute/NVDEC, which need the main client back).  The internal
     * controls (WRITE_INST_MEM / GET_STATIC_INFO on RM_SUBDEVICE) route through
     * the GSP's own internal client and are unaffected by rm->client. */
    u32 saved_client = rm->client;

    /* NVKMS owns NVC372 under its main client/device.  This must be allocated
     * before switching to nouveau's dedicated display client: an object can
     * successfully allocate in the wrong client yet have none of the
     * device-wide IMP/performance context, producing NV_OK plus an all-zero
     * bIsPossible response (the exact failure seen on hardware). */
    g_relight.stopped_at = "allocating main-client NVC372 RM-IMP object";
    if (!nv_rm_alloc(c, rm, RM_DEVICE, H_DISP_RMCTRL,
                     NVC372_DISPLAY_SW, NULL, 0)) {
        kerr("nv-disp", "main-client NVC372_DISPLAY_SW RM-IMP object refused (status %#x)",
             nv_last_alloc_status);
        return;
    }
    kinfo("nv-disp", "  RM-IMP control object open under main client %#x/device %#x, handle %#x",
          saved_client, RM_DEVICE, H_DISP_RMCTRL);

    rm->client = disp_client;

    /* 4. Allocate NV04_DISPLAY_COMMON "objcom" under the disp device, handle
     *    NVKM_RM_DISP, no params (disp.c:1540).  EVERY NV0073 control below
     *    targets this objcom handle (mirrors disp->rm.objcom). */
    g_relight.stopped_at = "allocating NV04_DISPLAY_COMMON objcom";
    u32 objcom = NVKM_RM_DISP;   /* the handle nouveau gives the objcom alloc */
    if (!nv_rm_alloc(c, rm, disp_device, objcom, NV04_DISPLAY_COMMON, NULL, 0)) {
        g_relight.disp_root_status = nv_last_alloc_status;
        kerr("nv-disp", "NV04_DISPLAY_COMMON objcom alloc refused (status %#x)",
             nv_last_alloc_status);
        goto done;
    }
    kinfo("nv-disp", "  objcom (NV04_DISPLAY_COMMON 0x0073) open, handle %#x", objcom);

    /* 5. Static info (window mask / head count) on the internal subdevice
     *    (disp.c:1545, 1483-1500). */
    disp_get_static_info_t si; memset(&si, 0, sizeof si);
    g_relight.stopped_at = "display static info";
    if (disp_ctrl(c, rm, RM_SUBDEVICE, NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO,
                  &si, sizeof si, "get static info")) {
        kinfo("nv-disp", "  windowPresentMask %#x numHeads %u", si.windowPresentMask, si.numHeads);
        g_relight.static_info_ok = true;
        g_relight.windows_present = si.windowPresentMask;
        g_relight.num_heads = si.numHeads;
    }

    /* 6. DP_SET_MANUAL_DISPLAYPORT once on the objcom (disp.c:1620-1633): disable
     *    GSP-RM's automatic watermark/CP-IRQ/defer handling so the manual modeset
     *    takes effect.  cmd 0x731365 (OGKM ctrl0073dp.h:1596), 4-byte params. */
    dp_set_manual_displayport_t man; memset(&man, 0, sizeof man);
    g_relight.stopped_at = "DP_SET_MANUAL_DISPLAYPORT";
    disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT,
              &man, sizeof man, "DP set manual");

    /* 7. r535_disp_init: allocate the 0xca70 disp-engine root under the disp
     *    DEVICE, handle 0xca70<<16, no params (disp.c:1460-1461). */
    g_relight.stopped_at = "allocating disp root (0xca70)";
    if (!nv_rm_alloc(c, rm, disp_device, H_DISP_ROOT, GB202_DISP, NULL, 0)) {
        g_relight.disp_root_status = nv_last_alloc_status;
        kerr("nv-disp", "disp root (0xca70) alloc refused (status %#x)", nv_last_alloc_status);
        goto done;
    }
    g_relight.disp_root_ok = true;

    /* -------- pick a display id to light (GET_SUPPORTED on the objcom) -------- */
    sys_get_supported_t sup; memset(&sup, 0, sizeof sup);
    g_relight.stopped_at = "GET_SUPPORTED";
    if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED,
                   &sup, sizeof sup, "system get supported") || !sup.displayMask) {
        kwarn("nv-disp", "no supported displays reported - aborting"); goto done;
    }
    g_relight.supported_mask = sup.displayMask;

    /* GET_SUPPORTED lists every POSSIBLE display slot; ask which are actually
     * CONNECTED (a sink present) and light one of THOSE - AUX/DP_CTRL to an empty
     * slot returns INVALID_ARGUMENT.  If the connect query fails or reports none,
     * fall back to the lowest supported bit (old behaviour). */
    u32 pick_mask = sup.displayMask;
    sys_get_connect_state_t cst; memset(&cst, 0, sizeof cst);
    cst.flags = NV0073_CONNECT_STATE_FLAGS_METHOD_CACHED;  /* as kdispIsDisplayConnected */
    cst.displayMask = sup.displayMask;   /* test the whole supported set */
    g_relight.stopped_at = "GET_CONNECT_STATE";
    if (disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE,
                  &cst, sizeof cst, "get connect state") && cst.displayMask) {
        pick_mask = cst.displayMask;
        kinfo("nv-disp", "  connected displays: %#x (of supported %#x)",
              cst.displayMask, sup.displayMask);
    } else {
        kwarn("nv-disp", "  connect-state query gave nothing (%#x) - using lowest supported bit",
              cst.displayMask);
    }
    g_relight.connected_mask = pick_mask;
    /* Display routing: do NOT arbitrarily pick the lowest
     * connected bit.  The boot/GOP head is already bound to a specific display,
     * and RM's SYSTEM_GET_ACTIVE(head0) reports it (this machine: 0x2000, while
     * the lowest connected bit is 0x200).  Attaching the wrong id to head0 makes
     * RM REJECT the SOR takeover (head raster runs but SOR stays detached) - the
     * exact observed failure.  So: query GET_ACTIVE for each hw head, and light
     * the display the boot head already owns.  Only fall back to the lowest
     * connected bit when NO head reports an active boot display.
     * (NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE 0x73010c, ctrl0073system.h:650-655.) */
    u32 active_ids[4] = { 0u, 0u, 0u, 0u };
    for (u32 h = 0u; h < 4u; h++) {
        sys_get_active_t ga; memset(&ga, 0, sizeof ga);
        ga.head = h;                              /* flags 0 = any active (boot or client) */
        if (disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE,
                      &ga, sizeof ga, "SYSTEM_GET_ACTIVE (select)") && ga.displayId) {
            kinfo("nv-disp", "  head%u active boot display %#x", h, ga.displayId);
            active_ids[h] = ga.displayId;
            if (display_id == 0u) {               /* first head with an active display wins */
                display_id = ga.displayId;
                g_relight.active_head = h;
            }
        }
    }

    /* Enumerate every connected sink and read that sink's own EDID.  Display IDs
     * are sparse bitmasks, so never infer connector count or monitor identity from
     * the numeric value.  This table is regenerated on every boot/hotplug pass. */
    memset(g_disp_sinks, 0, sizeof g_disp_sinks);
    g_disp_sink_count = 0;
    for (u32 bit_index = 0; bit_index < 32u &&
         g_disp_sink_count < NV_DISP_MAX_SINKS; bit_index++) {
        u32 id = 1u << bit_index;
        if (!(pick_mask & id)) continue;
        disp_sink_t *sink = &g_disp_sinks[g_disp_sink_count++];
        sink->display_id = id;
        sink->boot_head = 0xFFFFFFFFu;
        for (u32 h = 0; h < 4u; h++) {
            if (active_ids[h] == id) {
                sink->active_at_boot = true;
                sink->boot_head = h;
                break;
            }
        }
        disp_read_sink_edid(c, rm, objcom, id, sink);
        if (sink->valid) g_relight.edid_mask |= id;
    }
    g_relight.sink_count = g_disp_sink_count;
    kinfo("nv-disp", "  runtime topology: %u connected sink(s), capacity %u head(s)",
          g_disp_sink_count, g_relight.num_heads ? g_relight.num_heads : 4u);

    /* BOOT.CFG uses generated only:N policies, so the same code handles main,
     * second, third ... eighth screen.  Resolve N against the connected-sink
     * table (not sparse display-id bits); an unplugged selection falls back to
     * the active boot output rather than inventing a connector. */
    u32 only_index = 0xffffffffu;
    if (g_boot.display.display_mode >= KB_DISPLAY_ONLY_BASE)
        only_index = (u32)g_boot.display.display_mode - KB_DISPLAY_ONLY_BASE;
    else if (g_boot.display.display_mode == KB_DISPLAY_ONLY_OTHER)
        only_index = 1u;                         /* older loader compatibility */
    if (only_index < g_disp_sink_count) {
        display_id = g_disp_sinks[only_index].display_id;
        if (g_disp_sinks[only_index].active_at_boot)
            g_relight.active_head = g_disp_sinks[only_index].boot_head;
        only_one_output = true;
        kinfo("nv-disp", "  display policy: ONLY screen %u (displayId %#x)",
              only_index + 1u, display_id);
    } else if (only_index != 0xffffffffu) {
        kwarn("nv-disp", "  display policy requested screen %u but only %u connected; using active main",
              only_index + 1u, g_disp_sink_count);
    } else {
        kinfo("nv-disp", "  display policy: %s on %u connected screen(s)",
              g_boot.display.display_mode == KB_DISPLAY_MIRROR ? "DUPLICATE" : "EXTEND",
              g_disp_sink_count);
    }

    /* One VRAM surface backs the RGBW test on every head.  Derive its pitch and
     * used height from the exact modes in the newly read EDIDs, rather than from
     * the firmware GOP mode.  This is what lets hot-swapped 4K/8K/10K monitors
     * reach their own maximum raster instead of inheriting the old monitor's
     * pitch.  The mode selectors below still reject any timing that exceeds the
     * successfully allocated capacity. */
    if (have_surf) {
        u32 max_w = g_boot.fb.width, max_h = g_boot.fb.height;
        for (u32 i = 0; i < g_disp_sink_count; i++) {
            const edid_info_t *se = &g_disp_sinks[i].edid;
            for (int j = 0; g_disp_sinks[i].valid && j < se->mode_count; j++) {
                const edid_mode_t *sm = &se->modes[j];
                if (sm->flags & 0x80u) continue;
                if (sm->hactive > max_w) max_w = sm->hactive;
                if (sm->vactive > max_h) max_h = sm->vactive;
            }
        }
        u64 wanted_pitch = (((u64)max_w * 4ull) + 63ull) & ~63ull;
        u64 wanted_bytes = wanted_pitch * (u64)max_h;
        if (wanted_bytes > g_scanout_bytes) {
            /* The early reservation is intentionally only the boot sink's
             * tightly packed native raster: a fixed 4K floor starved the GR
             * context heap on this card.  Once every live EDID is known, grow
             * it here instead of silently capping a replacement 4K/8K sink to
             * the old monitor.  Memory objects belong to the main RM client,
             * while this whole display sequence is scoped to disp_client, so
             * switch clients for both ALLOC and FREE exactly as RM requires.
             * Keep the old allocation until the larger one has succeeded; a
             * fragmented heap therefore preserves the boot-capable surface.
             */
            u32 replacement = (g_scanout_handle == H_DISP_SCANOUT) ?
                              H_DISP_SCANOUT_ALT : H_DISP_SCANOUT;
            u64 replacement_fb = 0;
            rm->client = saved_client;
            bool grew = nv_vram_alloc_scanout(c, rm, replacement,
                                               wanted_bytes, &replacement_fb);
            u32 grow_status = nv_last_alloc_status;
            if (grew) {
                u32 old_handle = g_scanout_handle;
                if (g_scanout_ok &&
                    !nv_rm_free(c, rm, RM_DEVICE, old_handle))
                    kwarn("nv-disp", "  enlarged scanout is live but old handle %#x could not be freed",
                          old_handle);
                g_scanout_handle = replacement;
                g_scanout_fb = replacement_fb;
                g_scanout_bytes = wanted_bytes;
                g_scanout_status = 0u;
                g_scanout_ok = true;
                surf_fb = replacement_fb;
                surf_bytes = wanted_bytes;
                have_surf = true;
                kinfo("nv-disp", "  live EDIDs enlarged scanout to %ux%u: %llu B @ FB %#llx",
                      max_w, max_h, (unsigned long long)wanted_bytes,
                      (unsigned long long)replacement_fb);
            }
            rm->client = disp_client;
            if (!grew) {
                g_relight.rgb_alloc_status = grow_status;
                kwarn("nv-disp", "  EDID envelope %ux%u needs %llu B, live resize returned %#x; preserving %llu B surface",
                      max_w, max_h, (unsigned long long)wanted_bytes,
                      grow_status, (unsigned long long)g_scanout_bytes);
            }
        }
        if (wanted_bytes <= g_scanout_bytes) {
            surf_pitch = wanted_pitch;
            surf_bytes = wanted_bytes;
            kinfo("nv-disp", "  runtime scanout envelope: %ux%u, pitch %llu, used %llu/%llu B",
                  max_w, max_h, (unsigned long long)surf_pitch,
                  (unsigned long long)surf_bytes,
                  (unsigned long long)g_scanout_bytes);
        } else {
            kwarn("nv-disp", "  EDID envelope %ux%u needs %llu B, allocation has %llu B after resize; selectors will clamp safely",
                  max_w, max_h, (unsigned long long)wanted_bytes,
                  (unsigned long long)g_scanout_bytes);
            surf_bytes = g_scanout_bytes;
        }
    }
    if (display_id) {
        g_relight.display_from_active = true;
    } else {
        display_id = pick_mask & (~pick_mask + 1u);   /* fallback: lowest CONNECTED bit */
        g_relight.display_from_active = false;
        kwarn("nv-disp", "  no head reports an active boot display - falling back to lowest connected bit %#x", display_id);
    }
    g_relight.display_id = display_id;
    kinfo("nv-disp", "supported %#x connected %#x -> lighting displayId %#x (%s)",
          sup.displayMask, pick_mask, display_id,
          g_relight.display_from_active ? "from GET_ACTIVE boot head" : "fallback lowest-bit");

    /* NVIDIA 595 brackets one complete proposed topology, not one connector at
     * a time (BeginEndModeset in nvkms-modeset.c).  Build that final mask before
     * START and keep one bracket open while every head/SOR is programmed. */
    modeset_change_mask = display_id;
    if (!only_one_output) {
        u32 capacity = g_relight.num_heads ? g_relight.num_heads : 4u;
        u32 planned_heads = 1u;
        for (u32 i = 0; i < g_disp_sink_count && planned_heads < capacity; i++) {
            disp_sink_t *sink = &g_disp_sinks[i];
            if (!sink->valid || sink->display_id == display_id) continue;
            u32 window = planned_heads * 2u;
            if (g_relight.windows_present &&
                !(g_relight.windows_present & (1u << window))) continue;
            modeset_change_mask |= sink->display_id;
            planned_heads++;
        }
    }
    kinfo("nv-disp", "  one atomic DISPLAY_CHANGE topology mask %#x",
          modeset_change_mask);

    /* Select this sink's best exact detailed timing under the strongest classic
     * DP link we implement.  Once DPCD/card caps are known below, re-run the same
     * selector against the negotiated link ceiling. */
    disp_sink_t *selected_sink = NULL;
    for (u32 i = 0; i < g_disp_sink_count; i++)
        if (g_disp_sinks[i].display_id == display_id) {
            selected_sink = &g_disp_sinks[i];
            break;
        }
    if (selected_sink && selected_sink->valid) {
        ed = &selected_sink->edid;
        if (!disp_pick_best_mode(ed, DP_LINK_BW_8_10, 4u,
                                 surf_pitch, surf_bytes, &selected_mode))
            selected_mode = ed->native;
    } else if (!ed->valid || !selected_mode.pixel_clock_khz) {
        kwarn("nv-disp", "selected display %#x has no usable EDID; refusing to invent a raster",
              display_id);
        goto done;
    } else {
        kwarn("nv-disp", "selected display %#x uses loader EDID fallback (runtime read failed)",
              display_id);
    }
    g_relight.edid_ok = true;
    htotal = m->htotal; vtotal = m->vtotal;
    hactive = m->hactive; vactive = m->vactive;
    hsync_start = (u32)m->hactive + m->hsync_off;
    hsync_end   = hsync_start + m->hsync_w;
    vsync_start = (u32)m->vactive + m->vsync_off;
    vsync_end   = vsync_start + m->vsync_w;
    g_relight.mode_w = hactive; g_relight.mode_h = vactive;
    g_relight.mode_pclk_khz = m->pixel_clock_khz;
    kinfo("nv-disp", "  EDID candidate: %ux%u @ %u.%03u Hz, pclk %u kHz (not hardcoded)",
          hactive, vactive, m->refresh_mhz / 1000u, m->refresh_mhz % 1000u,
          m->pixel_clock_khz);
    u32 xbar_output = 0u;
    for (u32 bit = display_id; bit > 1u; bit >>= 1u) xbar_output++;
    g_relight.xbar_output = xbar_output;
    g_relight.xbar_a_before = nv_rd32(c, PDISP_XBAR_LINK_A(xbar_output));
    g_relight.xbar_b_before = nv_rd32(c, PDISP_XBAR_LINK_B(xbar_output));
    kinfo("nv-disp", "  connector XBAR out%u before assign: A %#x B %#x (low5 zero = unrouted)",
          xbar_output, g_relight.xbar_a_before, g_relight.xbar_b_before);

    /* -------- OR / SOR info -------- */
    or_get_info_t ori; memset(&ori, 0, sizeof ori);
    ori.displayId = display_id;
    g_relight.stopped_at = "OR_GET_INFO";
    if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO,
                   &ori, sizeof ori, "OR get info")) goto done;
    g_relight.or_info_ok = true;
    g_relight.or_index = ori.index; g_relight.or_type = ori.type; g_relight.or_protocol = ori.protocol;
    kinfo("nv-disp", "  OR index(SOR) %u type %u protocol %u", ori.index, ori.type, ori.protocol);
    /* NOTE: ori.index is NOT the SOR to use - OR_GET_INFO reports 0xFFFFFFFF
     * until DFP_ASSIGN_SOR binds one.  The real sor id is captured from
     * DFP_ASSIGN_SOR below (`u32 sor`).  Keep only the protocol here. */
    u32 sor_protocol = (ori.protocol == OR_PROTOCOL_SOR_DP_B) ? OR_PROTOCOL_SOR_DP_B
                                                              : OR_PROTOCOL_SOR_DP_A;

    /* ================================================================
     * DP bring-up in NVIDIA 595's order.  The previous version had two
     * bugs the on-silicon diag exposed: (a) it called DP_GET_CAPS with
     * sorIndex = ori.index, but OR_GET_INFO returns index 0xFFFFFFFF until a
     * SOR is actually assigned, and DFP_ASSIGN_SOR ran AFTER caps; (b) the
     * DP_CTRL cmd/data bits diverged from nouveau and no sink DPCD was read.
     * Correct order after the display object tree exists is: allocate the core
     * channel FIRST, assign the SOR, query the CARD's DP caps against that
     * assigned SOR, read the MONITOR's DPCD, min() the two ceilings, then train.
     *
     * This ordering is not optional when taking over a GOP-lit display.  The
     * actual 595.99.02 nvResumeDevEvo binary calls nvAllocCoreChannelEvo before
     * its NV0073_CTRL_CMD_DFP_ASSIGN_SOR controls.  The corresponding NVKMS
     * source explains why: assigning before core allocation makes RM unaware of
     * the boot display and it "ends up unrouting active SOR assignments".  Our
     * last hardware log showed exactly that result: link training succeeded and
     * the head raster advanced, but SOR0 stayed detached at ASSY/ARM 0x100.
     * ================================================================ */

    /* ================= MODESET BRACKET START (D1) =================
     * nvkms BEGIN_MODESET (nvkms-modeset.c:4074-4114 / nvRmBeginEndModeset,
     * nvkms-rm.c:1946): tell GSP-RM a display change to `display_id` is beginning
     * BEFORE the ENTIRE modeset sequence - DFP_ASSIGN_SOR, the DP detach, the
     * DP_CTRL training and DP_CONFIG_STREAM all run INSIDE this bracket, exactly as
     * nvkms brackets its whole BEGIN_MODESET...END_MODESET.  Previously START was
     * issued LATE (after detach/train/config-stream, just before the head/SOR push),
     * so GSP-RM never finalized the SOR pad/link bring-up on the bracketed change and
     * the SOR never transmitted (No Input).  END is issued after the final attach
     * UPDATE (below).  display_id and sor_protocol are already known here. */
    /* Changing from the firmware GOP timing to the monitor's maximum validated
     * timing is a real modeset even when this connector was already active.
     * Therefore every primary path enters BEGIN/END_MODESET.  The active SOR is
     * still not detached below; its ownership is rebuilt atomically with the
     * new head, DP stream and window state. */
    {
        sys_display_change_t dc; memset(&dc, 0, sizeof dc);
        dc.newDevices = modeset_change_mask; dc.enable = NV0073_DISPLAY_CHANGE_START;
        g_relight.stopped_at = "DISPLAY_CHANGE START (bracket open)";
        if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE,
                       &dc, sizeof dc, "DISPLAY_CHANGE START"))
            goto done;
        modeset_change_open = true;
    }

    /* Allocate display channels BEFORE DFP_ASSIGN_SOR.  NVIDIA 595's
     * nvResumeDevEvo does nvAllocCoreChannelEvo first (binary relocation at
     * +0xd4) and only later issues control 0x731152.  Kestrel previously opened
     * these channels in Phase B, after the assignment, reproducing the precise
     * hibernate/GOP takeover ordering that NVKMS warns can unroute the SOR. */
    dchan_t core, win;
    g_relight.stopped_at = "allocating core channel before DFP_ASSIGN_SOR (0xca7d)";
    if (!dchan_alloc(c, rm, &core, GB202_DISP_CORE_CHANNEL_DMA, 0,
                     CORE_USER_BASE, "core")) goto done;
    g_relight.core_chan_ok = true;
    g_relight.stopped_at = "allocating window channel before DFP_ASSIGN_SOR (0xca7e)";
    if (!dchan_alloc(c, rm, &win, GB202_DISP_WINDOW_CHANNEL_DMA, 0,
                     WINDOW_USER_BASE(0), "window")) goto done;
    g_relight.window_chan_ok = true;
    kinfo("nv-disp", "  NVIDIA 595 ordering: core/window channels open BEFORE DFP_ASSIGN_SOR");

    /* -------- (2) DFP_ASSIGN_SOR: bind a SOR to this display and learn the
     * ASSIGNED sor id.  The core channel is already allocated above, and this
     * assignment still precedes DP_GET_CAPS (whose sorIndex is the result). */
    u32 sor = (ori.index == 0xFFFFFFFFu) ? 0u : ori.index; /* provisional; DFP_ASSIGN_SOR overwrites */
    dfp_assign_sor_t asor; memset(&asor, 0, sizeof asor);
    asor.displayId = display_id;
    g_relight.sor_slot = -1;
    g_relight.stopped_at = "DFP_ASSIGN_SOR";
    if (disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DFP_ASSIGN_SOR, &asor, sizeof asor, "assign SOR")) {
        g_relight.assign_sor_ok = true;
        for (int i = 0; i < 4; i++)
            if (asor.sorAssignListWithTag[i].displayMask & display_id) {
                sor = (u32)i;
                g_relight.sor_slot = i;
                kinfo("nv-disp", "  SOR assigned: slot %d owns displayId", i);
            }
    } else {
        kwarn("nv-disp", "DFP_ASSIGN_SOR refused - proceeding with sor %u (caps/training may fail)", sor);
    }
    g_relight.xbar_expected = (sor + 1u) |
        ((sor_protocol == OR_PROTOCOL_SOR_DP_B) ? 0x10u : 0u);
    g_relight.xbar_a_assigned = nv_rd32(c, PDISP_XBAR_LINK_A(xbar_output));
    g_relight.xbar_b_assigned = nv_rd32(c, PDISP_XBAR_LINK_B(xbar_output));
    kinfo("nv-disp", "  connector XBAR out%u after assign: A %#x B %#x; expected route low5 %#x (%s)",
          xbar_output, g_relight.xbar_a_assigned, g_relight.xbar_b_assigned,
          g_relight.xbar_expected,
          (((sor_protocol == OR_PROTOCOL_SOR_DP_B ? g_relight.xbar_b_assigned
                                                  : g_relight.xbar_a_assigned) & 0x1fu)
             == g_relight.xbar_expected) ? "ROUTED" : "MISMATCH/UNROUTED");

    /* -------- (3) DP_GET_CAPS on the ASSIGNED sor -> the CARD's link ceiling.
     * ctrl->sorIndex is the assigned sor id (nouveau passes ~0 to query the
     * whole GPU at disp.c:1177, but we want the ceiling for THIS SOR).  The
     * maxLinkRate reply is a 2:0 bitfield code (ctrl0073dp.h:1754) that
     * dp_link_bw_code() maps to a SET_LINK_BW value; DP_GET_CAPS body per
     * disp.c:1166-1207. */
    dp_get_caps_t caps; memset(&caps, 0, sizeof caps);
    caps.sorIndex = sor;
    u32 card_bw = 0;             /* 0 == card ceiling unknown */
    g_relight.stopped_at = "DP_GET_CAPS";
    if (disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DP_GET_CAPS, &caps, sizeof caps, "DP get caps")) {
        g_relight.caps_ok = true; g_relight.max_link_rate = caps.maxLinkRate;
        if (caps.maxLinkRate != 0)           /* _NONE=0 means "no rate", ctrl0073dp.h:1755 */
            card_bw = dp_link_bw_code(caps.maxLinkRate);
        /* caps.UHBRSupported = Blackwell DP2.1 UHBR flag; logged for the diag but
         * not selected here (the 60Hz target mode needs only HBR2). */
        kinfo("nv-disp", "  CARD caps: maxLinkRate code %u -> bw %#x, UHBR %u, MST %u",
              caps.maxLinkRate, card_bw, caps.UHBRSupported, caps.bIsMultistreamSupported);
    }

    /* -------- (4) read the MONITOR's DPCD -> the SINK's link ceiling.
     * nv_dp_read_dpcd (concurrent agent, decl in nv.h) does the AUX read of
     * DPCD 0x0..0xF and parses max_link_rate_code (0x1), max_lanes (0x2 [4:0]),
     * ENHANCED_FRAME_CAP (0x2 bit7), TPS4 (0x3).  Needed for the conditional
     * enhanced-framing cmd bit at disp.c:978. */
    nv_dpcd_t dpcd; memset(&dpcd, 0, sizeof dpcd);
    g_relight.stopped_at = "read sink DPCD";
    bool have_dpcd = nv_dp_read_dpcd(c, rm, objcom, display_id, &dpcd) && dpcd.valid;
    g_relight.dpcd_valid = have_dpcd;
    g_relight.dpcd_aux_status = nv_last_control_status;   /* AUXCH_CTRL result (refused vs OK) */
    g_relight.dpcd_reply_type = dpcd.reply_type;          /* ACK/NACK/DEFER/TIMEOUT */
    g_relight.dpcd_got_bytes = dpcd.got_bytes;
    g_relight.dpcd_rate = dpcd.max_link_rate_code;
    g_relight.dpcd_lanes = dpcd.max_lanes;
    if (have_dpcd)
        kinfo("nv-disp", "  SINK DPCD: rev %#x maxRate %#x lanes %u enhframe %u tps4 %u",
              dpcd.rev, dpcd.max_link_rate_code, dpcd.max_lanes,
              dpcd.enhanced_framing, dpcd.tps4_supported);
    else
        kwarn("nv-disp", "  sink DPCD read failed - link config falls back to conservative defaults");

    /* -------- (5) choose the link config = min(card ceiling, sink ceiling).
     * The SET_LINK_BW / DPCD-0x1 rate codes (0x06<0x0a<0x14<0x1e) are monotone
     * in bandwidth, so numeric min() picks the lower rate.  If BOTH ceilings
     * are unknown, fall back to 0x06 (1.62G) exactly like r535_dp_release
     * (disp.c:1126-1130).  Lanes = min(4, sink max_lanes); 4 lanes is the
     * ceiling this core drives and also what the target mode needs.
     *
     * The mode we drive is the EDID NATIVE detailed timing (2560x1440@60,
     * pclk 241.5MHz) - that fits a 4-lane HBR2 (5.40G, code 0x14) link with
     * NO DSC, so we deliberately do NOT attempt DSC here (DSC is only needed
     * for the 240Hz mode; caps.DSC is read but unused). */
    /* Only accept a sink rate code that is a REAL SET_LINK_BW enum value
     * (ctrl0073dp.h:517-524).  A DPCD read that returned garbage would otherwise
     * feed an out-of-enum code straight into DP_CTRL, which GSP-RM rejects as
     * NV_ERR_INVALID_ARGUMENT (0x1f) - the exact symptom the last boot showed. */
    u32 sink_bw = 0;
    if (have_dpcd) switch (dpcd.max_link_rate_code) {
        case DP_LINK_BW_1_62: case 0x08u: case 0x09u: case DP_LINK_BW_2_70:
        case 0x0cu: case 0x10u: case DP_LINK_BW_5_40: case DP_LINK_BW_8_10:
            sink_bw = dpcd.max_link_rate_code; break;
        default: sink_bw = 0; break;   /* garbage/unknown -> ignore, use the card ceiling */
    }
    u32 link_bw;
    if (card_bw && sink_bw)      link_bw = (card_bw < sink_bw) ? card_bw : sink_bw;
    else if (card_bw)            link_bw = card_bw;
    else if (sink_bw)            link_bw = sink_bw;
    else                         link_bw = DP_LINK_BW_1_62;  /* disp.c:1130 fallback */

    /* Legal DP lane counts are ONLY 1, 2, 4 (ctrl0073dp.h:511-514; 8 is UHBR, not
     * used here).  Clamp the sink's DPCD count to the nearest legal value at or
     * below it - a raw value like 3 (garbage, or a partial AUX read) sent as
     * SET_LANE_COUNT is another INVALID_ARGUMENT source. */
    u32 lanes;
    if (have_dpcd && dpcd.max_lanes >= 1u) {
        u32 sl = dpcd.max_lanes;
        lanes = (sl >= 4u) ? 4u : (sl >= 2u) ? 2u : 1u;
    } else {
        lanes = 4u;              /* fallback: 4-lane ceiling, the target mode's need */
    }

    /* Enhanced framing is a CMD bit, gated on the sink's DPCD RC02 cap
     * (disp.c:978) - NOT a data bit (nouveau never sets DP_DATA enhanced). */
    bool enhanced = have_dpcd && dpcd.enhanced_framing;
    kinfo("nv-disp", "  CHOSEN link: %u lanes, bw code %#x, enhframe %u  (card_bw %#x sink_bw %#x)",
          lanes, link_bw, enhanced, card_bw, sink_bw);

    /* Now that both ends' real link ceilings are known, finalize the highest
     * exact advertised resolution+refresh pair.  Prefer uncompressed RGB8; when
     * it does not fit, intersect GPU DSC/FEC with the sink's DPCD DSC/FEC caps,
     * generate a real PPS and ask RM to validate the resulting implementation. */
    dp_sink_dsc_t primary_sink_dsc; memset(&primary_sink_dsc, 0, sizeof primary_sink_dsc);
    if (have_dpcd && dpcd.rev >= 0x14u)
        disp_read_dp_dsc_caps(c, rm, objcom, display_id, &primary_sink_dsc);
    if (selected_sink && selected_sink->valid) {
        edid_mode_t link_best;
        if (!disp_pick_best_dp_mode(c, rm, objcom, display_id, 0u,
                                   &selected_sink->edid, link_bw, lanes,
                                   enhanced, surf_pitch, surf_bytes, &caps,
                                   &primary_sink_dsc, &link_best, &primary_plan)) {
            kwarn("nv-disp", "  display %#x has no exact timing that fits %u-lane bw %#x, DSC/FEC and scanout allocation",
                  display_id, lanes, link_bw);
            goto done;
        } else {
            selected_mode = link_best;
            selected_sink->best_mode = link_best;
            htotal = m->htotal; vtotal = m->vtotal;
            hsync_start = (u32)m->hactive + m->hsync_off;
            hsync_end = hsync_start + m->hsync_w;
            vsync_start = (u32)m->vactive + m->vsync_off;
            vsync_end = vsync_start + m->vsync_w;
            hactive = m->hactive;
            vactive = m->vactive;
            g_relight.mode_w = m->hactive;
            g_relight.mode_h = m->vactive;
            g_relight.mode_pclk_khz = m->pixel_clock_khz;
            kinfo("nv-disp", "  display %#x selected %ux%u @ %u.%03u Hz, pclk %u kHz: highest valid EDID pair (%s)",
                  display_id, m->hactive, m->vactive, m->refresh_mhz / 1000u,
                  m->refresh_mhz % 1000u, m->pixel_clock_khz,
                  primary_plan.enabled ? "DSC+FEC" : "uncompressed");
        }
    }

    /* ================= DP DETACH-BEFORE-ATTACH (Phase B/C/D) =================
     * NVKMS forces a DP head to be SHUT DOWN in a separate, EARLIER interlocked
     * UPDATE before the attach ("DP always incompatible", nvkms-modeset.c:2455-2498;
     * distinct kickoff at nvkms-modeset.c:3243-3245).  After our SBR clears WPR2 the
     * SOR can come back RM-owned/stuck from the firmware's boot config; releasing it
     * BEFORE the fresh attach mirrors nouveau's r535_dp_release (disp.c:1124-1137:
     * train nr=0, then release).  The channels were deliberately opened BEFORE
     * DFP_ASSIGN_SOR above; the detach is issued as its OWN kickoff, distinct from
     * the later attach kickoffs (Update A / Update B).  This whole detach sits
     * INSIDE the DISPLAY_CHANGE bracket (START preceded channel allocation and
     * DFP_ASSIGN_SOR).
     * We must NOT merge OWNER_MASK=0 and OWNER_MASK=BIT(head) into one UPDATE. */

    /* The working driver explicitly treats every active DP head as incompatible
     * (nvkms-modeset.c:IsProposedModeSetStateOneApiHeadIncompatible): it must be
     * blanked and detached in an earlier update so DPlib can power down the old
     * link.  The previous skip for the GOP display violated that rule. */
    {
    /* ---- Phase B: SOR detach UPDATE (its OWN kickoff) ----
     * SOR_SET_CONTROL(sor) OWNER_MASK=NONE(0) (clca7d.h:311-313, offset 0x300+sor*0x20)
     * + HEAD_SET_DISPLAY_ID(head0,0)=0 (clca7d.h:874, 0x2020+head*0x800); then
     * NVCA7D_UPDATE (clca7d.h:80, 0x200).  This detaches the SOR/head assembly so a
     * stuck RM-owned SOR is released; kick it and wait for the channel to consume. */
    g_relight.stopped_at = "Phase B: SOR detach UPDATE";
    g_relight.sor_owner_arm_before_detach = nv_rd32(c, PDISP_SOR_OWNER_ARM(sor));
    /* BlankHeadEvo: disable OLUT and the base ISO surface, and clear the CA tile
     * and phywin assignments in the same interlocked shutdown update. */
    if (g_notifier_ok) {
        u32 cw = 0xffffffffu;
        for (u32 o = 0; o < 16u; o += 4u)
            disp_vram_wr32(c, g_notifier_fb + o, 0xdeadbeefu, &cw);
        dpush1(&core, CA7D_SET_SURFACE_ADDRESS_HI_NOTIFIER,
               (u32)(g_notifier_fb >> 32));
        dpush1(&core, CA7D_SET_SURFACE_ADDRESS_LO_NOTIFIER,
               ((u32)g_notifier_fb & 0xfffffff0u) |
               (CA7E_LO_ISO_TARGET_PHYSICAL_NVM << 2) | 1u);
        dpush1(&core, CA7D_SET_NOTIFIER_CONTROL,
               CA7D_NOTIFIER_CONTROL_NOTIFY_ENABLE);
    }
    dpush1(&core, CA7D_HEAD_SET_OLUT_CONTROL(0), 0u);
    dpush1(&core, CA7D_HEAD_SET_TILE_MASK(0), 0u);
    dpush1(&core, CA7D_WINDOW_SET_PHYSICAL(0), 0u);
    dpush1(&core, CA7D_SOR_SET_CONTROL(sor), 0u);             /* OWNER_MASK_NONE */
    dpush1(&core, CA7D_HEAD_SET_DISPLAY_ID(0, 0), 0u);
    dpush1(&core, CA7D_SET_INTERLOCK_FLAGS, 0u);
    dpush1(&core, CA7D_SET_WINDOW_INTERLOCK_FLAGS, 1u);       /* with window0 blank */
    dpush1(&core, CA7D_UPDATE, 1u);
    if (g_notifier_ok) dpush1(&core, CA7D_SET_NOTIFIER_CONTROL, 0u);

    dpush1(&win, CA7E_SET_SURFACE_ADDRESS_HI_ISO(0), 0u);
    dpush1(&win, CA7E_SET_SURFACE_ADDRESS_LO_ISO(0), 0u);
    dpush1(&win, CA7E_SET_INTERLOCK_FLAGS, 1u);
    dpush1(&win, CA7E_SET_WINDOW_INTERLOCK_FLAGS, 1u);
    dpush1(&win, CA7E_SW_SET_MCLK_SWITCH, 0u);
    dpush1(&win, CA7E_UPDATE, 1u);
    dpush1(&win, CA7E_SW_SET_MCLK_SWITCH, 1u);
    dkick(c, &core);
    dkick(c, &win);
    {
        u32 put = core.at << 2, get = 0;
        u32 wput = win.at << 2, wget = 0;
        for (int t = 0; t < 40; t++) {
            get = nv_rd32(c, CORE_USER_BASE + 0x4u);
            wget = nv_rd32(c, WINDOW_USER_BASE(0) + 0x4u);
            if (get == put && wget == wput) break;
            u64 dl = g_uptime_ms + 5; while (g_uptime_ms < dl) timer_udelay(500);
        }
        if (g_notifier_ok) {
            for (u32 t = 0; t < 100u; t++) {
                if (disp_vram_rd32(c, g_notifier_fb) != 0xdeadbeefu) break;
                u64 dl = g_uptime_ms + 2u;
                while (g_uptime_ms < dl) timer_udelay(500);
            }
        }
        g_relight.detach_kicked = (get == put && wget == wput);
        g_relight.sor_owner_arm_after_detach = nv_rd32(c, PDISP_SOR_OWNER_ARM(sor));
        g_relight.sor_released = !(g_relight.sor_owner_arm_after_detach & 0xFFu);
        kinfo("nv-disp", "  full blank/detach consumed core %#x/%#x window %#x/%#x; SOR%u ARM %#x->%#x (%s)",
              get, put, wget, wput, sor,
              g_relight.sor_owner_arm_before_detach, g_relight.sor_owner_arm_after_detach,
              g_relight.sor_released ? "released" : "still owned");
        if (!g_relight.detach_kicked) goto done;
    }

    /* ---- Phase C: power down the old external-DP link after the detach UPDATE ----
     * Match ConnectorImpl::powerdownLink() exactly for an external monitor: put the
     * sink in D3 and train lane_count=0/RBR.  Do NOT issue DP_MAIN_LINK_CTRL here.
     * The working driver uses that separate GPU power control for eDP panel-power
     * transitions (DeviceImpl::setPanelPowerParams), not for normal external-DP
     * detach/attach. */
    g_relight.stopped_at = "Phase C: DP link power-down";
    nv_dp_write_dpcd(c, rm, objcom, display_id, 0x600u, 0x2u);    /* sink -> D3 */
    {
        dp_ctrl_t dpd; memset(&dpd, 0, sizeof dpd);
        dpd.subDeviceInstance = 0;
        dpd.displayId = display_id;
        dpd.cmd  = DP_CMD_SET_LANE_COUNT | DP_CMD_SET_LINK_BW;         /* r535_dp_train_target */
        dpd.data = (0u << DP_DATA_LANE_COUNT_SHIFT)                    /* nr=0 -> release */
                 | (DP_LINK_BW_1_62 << DP_DATA_LINK_BW_SHIFT)         /* disp.c:1130 fallback bw */
                 | (0u << DP_DATA_TARGET_SHIFT);                       /* TARGET_SINK */
        u32 got = 0;
        nv_rm_control(c, rm, objcom, NV0073_CTRL_CMD_DP_CTRL,
                      &dpd, sizeof dpd, &dpd, sizeof dpd, &got);
        g_relight.detach_dp_release_err = dpd.err;
        kinfo("nv-disp", "  Phase C DP_CTRL lane=0 release: err %#x (nouveau r535_dp_release)", dpd.err);
    }

    /* ---- Phase D: restore the SOR assignment AFTER shutdown/release ----
     * This was the missing half of NVIDIA's resume sequence.  nvResumeDevEvo does:
     *   CacheSorAssignList -> allocate core -> shut down heads -> RestoreSorAssignList.
     * RestoreSorAssignList calls DFP_ASSIGN_SOR with sorExcludeMask=~BIT(oldSor),
     * forcing RM to restore the cached connector->SOR mapping before link training.
     * Kestrel used to assign before shutdown and then run the release path, leaving
     * the final attach with no live RM/xbar assignment. */
    {
        dfp_assign_sor_t restore; memset(&restore, 0, sizeof restore);
        restore.displayId = display_id;
        restore.sorExcludeMask = (u8)~(1u << sor);
        g_relight.stopped_at = "Phase D: restore SOR assignment after release";
        g_relight.restore_sor_ok =
            disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DFP_ASSIGN_SOR,
                      &restore, sizeof restore, "restore SOR after release");
        if (!g_relight.restore_sor_ok) {
            kwarn("nv-disp", "post-release SOR restore failed; refusing an unrouted attach");
            goto done;
        }

        u32 restored_sor = 0xFFFFFFFFu;
        for (u32 i = 0; i < 4u; i++) {
            if (restore.sorAssignListWithTag[i].displayMask & display_id) {
                restored_sor = i;
                break;
            }
        }
        if (restored_sor == 0xFFFFFFFFu) {
            kwarn("nv-disp", "post-release SOR restore returned no route for displayId %#x", display_id);
            goto done;
        }
        if (restored_sor != sor) {
            kwarn("nv-disp", "RM changed restored SOR %u -> %u; using restored assignment",
                  sor, restored_sor);
            sor = restored_sor;
            g_relight.sor_slot = (int)sor;
        }
        g_relight.xbar_expected = (sor + 1u) |
            ((sor_protocol == OR_PROTOCOL_SOR_DP_B) ? 0x10u : 0u);
        g_relight.xbar_a_assigned = nv_rd32(c, PDISP_XBAR_LINK_A(xbar_output));
        g_relight.xbar_b_assigned = nv_rd32(c, PDISP_XBAR_LINK_B(xbar_output));
        kinfo("nv-disp", "  restored SOR%u after release (exclude %#x); XBAR A %#x B %#x",
              sor, restore.sorExcludeMask,
              g_relight.xbar_a_assigned, g_relight.xbar_b_assigned);
    }
    }

    /* Wake the sink receiver: DPCD DP_SET_POWER (0x600) = D0 (0x1).  A receiver in
     * a low-power state will not show video even with a valid trained link + MSA
     * (nouveau_dp.c:423-428).  Best-effort - a sink already in D0 no-ops. */
    nv_dp_write_dpcd(c, rm, objcom, display_id, 0x600u, 0x1u);

    /* -------- (6) train the link (GSP trains internally) --------
     * cmd/data per r535_dp_train_target (disp.c:960-1020):
     *   cmd  = SET_LANE_COUNT(TRUE) | SET_LINK_BW(TRUE) | TRAIN_PHY_REPEATER(YES)
     *          [ | SET_ENHANCED_FRAMING(TRUE) iff sink caps ]      (disp.c:968-979)
     *   data = SET_LANE_COUNT(lanes) | SET_LINK_BW(bw) | TARGET(target) (disp.c:971-973)
     * Target loop runs from the LTTPR count down to 0 (disp.c:1025).  We do NOT
     * read the LTTPR count (DPCD 0xF0000 range; nv_dp_read_dpcd only returns
     * 0x0..0xF), so we train the sink directly at TARGET=0.  TODO: if an LTTPR
     * is present it must be trained first (targets N..1) - unpinned here.
     * -EAGAIN/retryTimeMs means "sleep retryTimeMs and resend the SAME request"
     * (disp.c:1000-1009); only err==0 with no pending retry is a clean pass. */
    /* cmd per the REAL DP library (dp_evoadapter.cpp:1002-1041), NOT nouveau's
     * simplified r535_dp_train_target: SET_LANE_COUNT | SET_LINK_BW, plus
     * ENHANCED_FRAMING only if the sink caps say so.  CRITICAL: TRAIN_PHY_REPEATER
     * is set ONLY when LTTPR repeaters are present AND the DPLib/RM counts agree
     * (evoadapter :1025-1039: "if LTTPR count is out of sync ... do not link train
     * LTTPRs"); with no repeaters it trains the SINK directly with NO repeater bit
     * and targetIndex=SINK.  Our old code set TRAIN_PHY_REPEATER UNCONDITIONALLY
     * with target=SINK - a contradiction GSP-RM rejects as INVALID_ARGUMENT (0x1f),
     * the exact failure the last boots showed.  We do not read/train LTTPRs, so we
     * never set the bit and always target the sink. */
    u32 dp_cmd = DP_CMD_SET_LANE_COUNT | DP_CMD_SET_LINK_BW;
    if (enhanced) dp_cmd |= DP_CMD_SET_ENHANCED_FRAMING;      /* evoadapter :1014 */
    if (primary_plan.enabled) dp_cmd |= DP_CMD_ENABLE_FEC;
    g_relight.dp_sent_cmd = dp_cmd;

    /* Rate fallback, as the real DP library does on CR failure (evoadapter
     * :1058-1075 restores a lower rate and retries): try the chosen ceiling
     * first, then each lower valid link rate down to 1.62G.  Our DPCD read
     * failed, so we do not know the sink's true max - stepping down finds a rate
     * both ends accept.  Lanes stay at the chosen count. */
    static const u32 bw_ladder[] = { DP_LINK_BW_8_10, DP_LINK_BW_5_40,
                                     DP_LINK_BW_2_70, DP_LINK_BW_1_62 };
    const u32 target = 0;                                     /* TARGET_SINK */
    dp_ctrl_t dp; bool trained = false;
    int total_attempts = 0;
    /* This is now a full bracketed timing change on both inherited and newly
     * attached outputs.  Retrain the physical link for the selected mode; reusing
     * GOP training while changing the raster/DSC state would leave an unvalidated
     * stream and violate the monitor+cable maximum-mode contract. */
    g_relight.stopped_at = "DP link training";
    for (unsigned r = 0; r < sizeof bw_ladder / sizeof bw_ladder[0] && !trained; r++) {
        u32 try_bw = bw_ladder[r];
        if (try_bw > link_bw) continue;                       /* never exceed the ceiling */
        u32 dp_data = (lanes << DP_DATA_LANE_COUNT_SHIFT)
                    | (try_bw << DP_DATA_LINK_BW_SHIFT)
                    | (target << DP_DATA_TARGET_SHIFT);
        g_relight.dp_sent_data = dp_data;
        for (int attempt = 0; attempt < 8 && !trained; attempt++) {
            total_attempts++;
            g_relight.dp_attempts = total_attempts;
            memset(&dp, 0, sizeof dp);
            dp.subDeviceInstance = 0;
            dp.displayId = display_id;                        /* BIT(index) */
            dp.retryTimeMs = 0;
            dp.cmd = dp_cmd;
            dp.data = dp_data;
            u32 got = 0;
            bool ok = nv_rm_control(c, rm, objcom, NV0073_CTRL_CMD_DP_CTRL,
                                    &dp, sizeof dp, &dp, sizeof dp, &got);
            g_relight.dp_last_err = (ok ? 0u : 0x80000000u) | (dp.err & 0x7FFFFFFFu);
            g_relight.dp_last_retry_ms = dp.retryTimeMs;
            g_relight.dp_ctrl_status = nv_last_control_status;
            kinfo("nv-disp", "  DP_CTRL bw=%#x lanes=%u attempt %d: rpc_ok=%d err=%#x "
                             "retryMs=%u cmd=%#x data=%#x ctrl_status=%#x replylen=%u",
                  try_bw, lanes, total_attempts, ok, dp.err, dp.retryTimeMs, dp.cmd, dp.data,
                  nv_last_control_status, nv_last_control_replylen);
            if (ok && dp.err == 0 && dp.retryTimeMs == 0) {
                trained = true; link_bw = try_bw; break;      /* clean pass at this rate */
            }
            if (dp.retryTimeMs) {                             /* DEFER: wait, retry SAME rate */
                u64 dl = g_uptime_ms + dp.retryTimeMs;
                while (g_uptime_ms < dl) timer_udelay(500);
                continue;
            }
            /* Hard failure at this rate (refused, or err with no retry): stop
             * hammering it and drop to the next lower rate. */
            break;
        }
    }
    /* (B) HARDWARE FINDING (2026-09-04): committing our modeset with SOR_SET_CONTROL
     * SEIZES the SOR and DROPS the firmware's trained link - the monitor then
     * reports "No Input" (signal gone, not just frozen).  So the earlier
     * assumption that we could reuse the firmware's link WITHOUT retraining is
     * FALSE: taking the SOR resets it, so a successful DP_CTRL train is MANDATORY
     * to get a signal.  We still proceed to the modeset on a failed train (for
     * diagnostics, and it is safe/recoverable - the panel is already frozen and
     * the boot auto-reboots), but until DP_CTRL returns clean the panel will read
     * "No Input".  g_relight.trained records whether OUR training succeeded. */
    g_relight.trained = trained;
    if (!trained) {
        kwarn("nv-disp", "our DP_CTRL training did not complete (status %#x, last err %#x retryMs %u); refusing an unvalidated stream",
              g_relight.dp_ctrl_status, g_relight.dp_last_err,
              g_relight.dp_last_retry_ms);
        goto done;
    }
    kinfo("nv-disp", "  link trained: %u lanes, bw code %#x, enhframe %u",
          lanes, link_bw, enhanced);

    /* If physical training fell down the rate ladder, the former mode/DSC plan
     * was computed against too much bandwidth.  Re-select at the proven rate.
     * This can move to a lower pair or turn DSC on. */
    bool trained_with_fec = (dp_cmd & DP_CMD_ENABLE_FEC) != 0u;
    if (selected_sink && selected_sink->valid) {
        edid_mode_t trained_mode;
        dp_mode_plan_t trained_plan; memset(&trained_plan, 0, sizeof trained_plan);
        if (!disp_pick_best_dp_mode(c, rm, objcom, display_id, 0u,
                                   &selected_sink->edid, link_bw, lanes,
                                   enhanced, surf_pitch, surf_bytes, &caps,
                                   &primary_sink_dsc, &trained_mode,
                                   &trained_plan))
            goto done;
        selected_mode = trained_mode; primary_plan = trained_plan;
        selected_sink->best_mode = trained_mode;
        htotal = m->htotal; vtotal = m->vtotal;
        hactive = m->hactive; vactive = m->vactive;
        hsync_start = (u32)m->hactive + m->hsync_off;
        hsync_end = hsync_start + m->hsync_w;
        vsync_start = (u32)m->vactive + m->vsync_off;
        vsync_end = vsync_start + m->vsync_w;
    }
    if (primary_plan.enabled && !trained_with_fec) {
        dp_ctrl_t fec_train; memset(&fec_train, 0, sizeof fec_train);
        fec_train.displayId = display_id;
        fec_train.cmd = DP_CMD_SET_LANE_COUNT | DP_CMD_SET_LINK_BW |
                        (enhanced ? DP_CMD_SET_ENHANCED_FRAMING : 0u) |
                        DP_CMD_ENABLE_FEC;
        fec_train.data = (lanes << DP_DATA_LANE_COUNT_SHIFT) |
                         (link_bw << DP_DATA_LINK_BW_SHIFT);
        u32 got = 0;
        if (!nv_rm_control(c, rm, objcom, NV0073_CTRL_CMD_DP_CTRL,
                           &fec_train, sizeof fec_train, &fec_train,
                           sizeof fec_train, &got) || fec_train.err)
            goto done;
        trained_with_fec = true;
    }
    if (primary_plan.enabled) {
        dp_configure_fec_t fec; memset(&fec, 0, sizeof fec);
        fec.displayId = display_id;
        fec.bEnableFec = 1u;
        if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DP_CONFIGURE_FEC,
                       &fec, sizeof fec, "primary DP enable GPU FEC"))
            goto done;
        if (!nv_dp_write_dpcd(c, rm, objcom, display_id, 0x160u, 1u))
            goto done;
    }

    /* -------- configure the SST stream with a COMPUTED watermark, override=1 ----
     * CRITICAL (SOR trace): bEnableOverride=0 is a no-op in the GSP model - RM does
     * NOT program the stream-formatter watermark itself, so with override off the
     * SOR sends a trained link with NO active symbols = black.  Compute the
     * watermark/hBlankSym/vBlankSym exactly as nv50_sor_dp_watermark_sst and push
     * with bEnableOverride=1 (OGKM ctrl0073dp.h:1445-1447; disp.c:1830). */
    {
        dp_config_stream_t cs; memset(&cs, 0, sizeof cs);
        cs.head = 0; cs.sorIndex = sor;
        cs.dpLink = (sor_protocol == OR_PROTOCOL_SOR_DP_B) ? 1 : 0;
        cs.bMST = 0;
        cs.SST.bEnhancedFraming = enhanced ? 1 : 0;
        cs.SST.tuSize = primary_plan.enabled ? primary_plan.tu : 64u;
        u32 wm = 0, hbsym = 0, vbsym = 0;
        bool wm_ok = false;
        if (primary_plan.enabled) {
            wm = primary_plan.wm; hbsym = primary_plan.hblank_sym;
            vbsym = primary_plan.vblank_sym; wm_ok = true;
        } else {
            wm_ok = disp_dp_watermark_sst(link_bw, lanes, m->pixel_clock_khz,
                                          hactive, htotal, enhanced,
                                          &wm, &hbsym, &vbsym);
        }
        if (wm_ok) {
            cs.bEnableOverride = 1;
            cs.hBlankSym = hbsym; cs.vBlankSym = vbsym; cs.SST.waterMark = wm;
            g_relight.wm_override = true; g_relight.wm_value = wm;
            kinfo("nv-disp", "  SST watermark: wm %u hBlankSym %u vBlankSym %u (override=1)",
                  wm, hbsym, vbsym);
        } else {
            cs.bEnableOverride = 0;   /* computation out of range - fall back to RM */
            kwarn("nv-disp", "  SST watermark computation failed - override=0 fallback");
        }
        if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_DP_CONFIG_STREAM,
                       &cs, sizeof cs, "DP config stream"))
            goto done;
    }

    /* Complete RM routing, mode selection, link training and stream setup for
     * every secondary before submitting any modeset method.  NVKMS constructs
     * the entire proposed topology first; it never promotes one connector at a
     * time inside BeginEndModeset(). */
    if (!only_one_output) {
        u32 used_sor_mask = 1u << sor;
        u32 next_head = 1u;
        for (u32 i = 0; i < g_disp_sink_count &&
                        next_head < 4u && secondary_count < 3u; i++) {
            disp_sink_t *sink = &g_disp_sinks[i];
            if (!sink->valid || sink->display_id == display_id) continue;
            u32 window = next_head * 2u;
            if (disp_prepare_secondary(c, rm, objcom, sink, next_head, window,
                                       surf_pitch, surf_bytes, &used_sor_mask,
                                       &secondary[secondary_count]))
                secondary_count++;
            next_head++;
        }
    }
    kinfo("nv-disp", "prepared one atomic topology: primary + %u secondary head(s)",
          secondary_count);

    u32 connected_sink_count = 0u;
    for (u32 i = 0; i < g_disp_sink_count; i++)
        if (g_disp_sinks[i].valid) connected_sink_count++;
    if (connected_sink_count && secondary_count + 1u != connected_sink_count) {
        kerr("nv-disp", "only %u/%u connected sinks have a complete route/link/stream; refusing a partial color test",
             secondary_count + 1u, connected_sink_count);
        goto done;
    }

    /* RM-IMP is a proposal validator, not part of the display-change commit.
     * NVIDIA calls it from AssignAndValidateProposedModeSet(), then opens
     * BEGIN_MODESET only after validation succeeds (nvkms-modeset.c:3995-4043).
     * Kestrel previously left DISPLAY_CHANGE START open here; RM accepted the
     * RPC but returned an all-zero impossible result for every rate, including
     * three 1440p60 heads.  Close the teardown/link-preparation phase before
     * validating the final topology. */
    if (modeset_change_open) {
        sys_display_change_t dc; memset(&dc, 0, sizeof dc);
        dc.newDevices = modeset_change_mask;
        dc.enable = NV0073_DISPLAY_CHANGE_END;
        g_relight.stopped_at = "DISPLAY_CHANGE END before RM-IMP";
        if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE,
                       &dc, sizeof dc, "DISPLAY_CHANGE END (pre-IMP)"))
            goto done;
        modeset_change_open = false;
    }

    /* This is the Linux driver's mandatory whole-topology feasibility and
     * performance-state transaction.  Never submit a cold multi-head modeset
     * that RM-IMP says the hardware cannot sustain. */
    g_relight.stopped_at = "C372 IS_MODE_POSSIBLE / RM-IMP";
    bool imp_ok = disp_imp_validate(c, rm, saved_client, H_DISP_RMCTRL,
                                    m, &primary_plan, secondary,
                                    secondary_count, si.windowPresentMask);
    bool imp_mode_fallback = false;
    for (u32 attempt = 0u; !imp_ok && attempt < 64u; attempt++) {
        /* Keep every connected head and its maximum resolution.  Reduce exactly
         * one uncompressed DP head to its next lower real EDID refresh, choosing
         * the currently heaviest stream first, then resubmit the whole atomic
         * topology.  Thus one boot explores the useful three-head bandwidth
         * matrix instead of asking the user to reboot for each rate. */
        edid_mode_t candidate; memset(&candidate, 0, sizeof candidate);
        u32 candidate_slot = 0xffffffffu; /* 0=primary, 1+i=secondary */
        u32 heaviest_pclk = 0u;
        if (selected_sink && !primary_plan.enabled &&
            disp_pick_next_lower_dp_mode(&selected_sink->edid, m,
                                         link_bw, lanes, surf_pitch,
                                         surf_bytes, &candidate)) {
            candidate_slot = 0u;
            heaviest_pclk = m->pixel_clock_khz;
        }
        for (u32 i = 0u; i < secondary_count; i++) {
            disp_secondary_plan_t *sp = &secondary[i];
            edid_mode_t next; memset(&next, 0, sizeof next);
            if (sp->is_hdmi || sp->dp_plan.enabled || !sp->sink ||
                !disp_pick_next_lower_dp_mode(&sp->sink->edid, &sp->mode,
                                              sp->link_bw, sp->link_lanes,
                                              surf_pitch, surf_bytes, &next))
                continue;
            if (candidate_slot == 0xffffffffu ||
                sp->mode.pixel_clock_khz > heaviest_pclk) {
                candidate = next;
                candidate_slot = i + 1u;
                heaviest_pclk = sp->mode.pixel_clock_khz;
            }
        }
        if (candidate_slot == 0xffffffffu) break;
        if (candidate_slot == 0u) {
            kinfo("nv-disp", "  IMP retry %u: head0 keeps %ux%u, EDID refresh %u.%03u -> %u.%03u Hz",
                  attempt + 1u, m->hactive, m->vactive,
                  m->refresh_mhz / 1000u, m->refresh_mhz % 1000u,
                  candidate.refresh_mhz / 1000u, candidate.refresh_mhz % 1000u);
            selected_mode = candidate;
            selected_sink->best_mode = candidate;
        } else {
            disp_secondary_plan_t *sp = &secondary[candidate_slot - 1u];
            kinfo("nv-disp", "  IMP retry %u: head%u keeps %ux%u, EDID refresh %u.%03u -> %u.%03u Hz",
                  attempt + 1u, sp->head, sp->mode.hactive, sp->mode.vactive,
                  sp->mode.refresh_mhz / 1000u, sp->mode.refresh_mhz % 1000u,
                  candidate.refresh_mhz / 1000u, candidate.refresh_mhz % 1000u);
            sp->mode = candidate;
            sp->sink->best_mode = candidate;
        }
        imp_mode_fallback = true;
        imp_ok = disp_imp_validate(c, rm, saved_client, H_DISP_RMCTRL,
                                   m, &primary_plan, secondary,
                                   secondary_count, si.windowPresentMask);
    }
    if (!imp_ok) {
        kerr("nv-disp", "RM-IMP rejected the proposed display topology; not committing it");
        goto done;
    }

    /* Begin the final atomic commit only after IMP has accepted the exact
     * all-head proposal.  This restores NVIDIA's validate -> BEGIN_MODESET ->
     * hardware update ordering while retaining the already completed safe
     * teardown/link preparation required by this standalone driver. */
    {
        sys_display_change_t dc; memset(&dc, 0, sizeof dc);
        dc.newDevices = modeset_change_mask;
        dc.enable = NV0073_DISPLAY_CHANGE_START;
        g_relight.stopped_at = "DISPLAY_CHANGE START after RM-IMP";
        if (!disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE,
                       &dc, sizeof dc, "DISPLAY_CHANGE START (post-IMP commit)"))
            goto done;
        modeset_change_open = true;
    }

    if (imp_mode_fallback) {
        /* CONFIG_STREAM was calculated for the original timings.  The already
         * trained links can carry every lower mode, but each stream formatter
         * needs a fresh watermark before the atomic core promotion. */
        if (!disp_reconfigure_uncompressed_dp_stream(c, rm, objcom, 0u, sor,
                sor_protocol, link_bw, lanes, enhanced, m,
                "primary DP config stream (IMP fallback)"))
            goto done;
        for (u32 i = 0u; i < secondary_count; i++) {
            disp_secondary_plan_t *sp = &secondary[i];
            if (sp->is_hdmi) continue;
            if (sp->dp_plan.enabled ||
                !disp_reconfigure_uncompressed_dp_stream(c, rm, objcom,
                    sp->head, sp->sor, sp->protocol, sp->link_bw,
                    sp->link_lanes, sp->link_enhanced, &sp->mode,
                    "secondary DP config stream (IMP fallback)"))
                goto done;
        }
        htotal = m->htotal; vtotal = m->vtotal;
        hactive = m->hactive; vactive = m->vactive;
        hsync_start = (u32)m->hactive + m->hsync_off;
        hsync_end = hsync_start + m->hsync_w;
        vsync_start = (u32)m->vactive + m->vsync_off;
        vsync_end = vsync_start + m->vsync_w;
        kinfo("nv-disp", "RM-IMP accepted all %u connected displays after in-boot EDID refresh search",
              connected_sink_count);
    }

    /* (core + window channels were opened earlier, before the Phase B SOR detach.) */

    /* ================= (C) RGB OUTPUT TEST: a KNOWN colour at a KNOWN address ==
     * The user's explicit ask: make the scanned-out surface show a solid,
     * unambiguous colour so the connected panels visibly PROVE the output works.
     *
     * SCANOUT ADDRESS - why not g_boot.fb.base directly:  SET_SURFACE_ADDRESS's
     * TARGET_PHYSICAL_NVM (clca7e.h; CA7E_LO_ISO_TARGET_PHYSICAL_NVM below) wants
     * a VRAM-LOCAL byte offset, but g_boot.fb.base is the firmware GOP's PHYSICAL
     * CPU/BAR address (bootinfo.h:51, "physical address of the framebuffer").  On
     * a GSP card BAR1 is RM-owned and virtualised (nv_chan.c:1330-1336: "usually
     * NOT 1:1"), so fb.base - c->vram_base is NOT a dependable VRAM offset - the
     * firmware's BAR1 page tables that produced fb.base are gone once GSP re-inits
     * BAR1, and the boot fb may even live on the Intel iGPU.  Pointing PHYSICAL_NVM
     * at that address is exactly what produced garbage before.  So we take the
     * reliable path the task allows: allocate a DEDICATED contiguous VRAM surface
     * (nv_vram_alloc returns a real VRAM-local FB offset), fill it with a solid
     * colour by writing VRAM through the BAR0 PRAMIN window (disp_vram_fill, the
     * nv_chan.c nv_fb_wr32 mechanism), and scan THAT out - a KNOWN address holding
     * a KNOWN colour.
     *
     * Colour: pure RED built from the framebuffer's OWN detected channel shifts
     * (g_boot.fb.*_shift), so it is correct for either 8-8-8 layout - for the
     * X8R8G8B8/BGRX case (red_shift 16) this is 0x00FF0000, exactly the task's
     * reference value.  red_px + the surface were computed/allocated EARLY (see
     * above, before the object tree) so the alloc runs on fresh VRAM. */

    /* Also paint the CPU-visible boot framebuffer red (the memory the desktop
     * composites into) - satisfies "fill the visible framebuffer from the CPU"
     * and covers the case where the boot fb IS what a panel still shows. */
    if (g_boot.fb.base && g_boot.fb.size) {
        volatile u32 *cpu = (volatile u32 *)phys_to_virt(g_boot.fb.base);
        u64 npix = g_boot.fb.size / 4u;
        for (u64 i = 0; i < npix; i++) cpu[i] = red_px;
        kinfo("nv-disp", "  RGB test: filled boot fb (%llu px @ virt %p) with red %#x",
              (unsigned long long)npix, (void *)cpu, red_px);
    }

    /* The early guard guarantees this is a real RM-returned local FB offset.
     * There is intentionally no GOP-address fallback: PHYSICAL_NVM must never
     * receive a CPU physical/BAR address. */
    disp_vram_fill(c, surf_fb, surf_bytes, red_px);
    u64 scanout_fb = surf_fb;
    bool scanout_vram = true;
    g_relight.rgb_test_shown = true;
    kinfo("nv-disp", "  RGB test: VRAM surface %llu B @ FB offset %#llx filled red %#x, "
                     "TARGET=PHYSICAL_NVM pitch %llu",
          (unsigned long long)surf_bytes, (unsigned long long)surf_fb, red_px,
          (unsigned long long)surf_pitch);

    g_relight.stopped_at = "pushing the atomic core topology and decoupled flips";

    /* Exact NVKMS first-core transaction (nvkms-modeset.c 3631-3665,
     * 3466-3533): InitWindowMapping queues all ownership/usage methods and marks
     * windowMappingChanged.  Every enabled head/SOR is then queued into that
     * SAME core update.  Because ownership changed, flips are decoupled: the
     * core update has no window interlock, completes first, and the window
     * surfaces are promoted afterward in a separate windows-only update. */

    /* Capture the hardware raster counters before the modeset.  Unlike a consumed
     * pushbuffer, movement in LOADV is direct evidence that a raster is running. */
    g_relight.rg_loadv_before = nv_rd32(c, PDISP_RG_IN_LOADV_COUNTER(0));
    g_relight.postcomp_loadv_before = nv_rd32(c, PDISP_POSTCOMP_LOADV_COUNTER(0));
    g_relight.crashlock_before = nv_rd32(c, PDISP_RG_CRASHLOCK_COUNTER(0));

    /* Queue first-channel usage bounds and fixed window ownership.  Do not kick
     * yet: the Linux driver folds these methods into the full core modeset. */
    /* EvoInitChannelC9 initializes the desktop colour state for every API head,
     * including heads that did not inherit a firmware/GOP modeset. */
    for (u32 i = 0; i < 4u; i++) {
        dpush1(&core, CA7D_HEAD_SET_DESKTOP_COLOR_ALPHA_RED(i), 0x000000ffu);
        dpush1(&core, CA7D_HEAD_SET_DESKTOP_COLOR_GREEN_BLUE(i), 0u);
    }
    for (u32 i = 0; i < 8u; i++) {
        bool active_window = (i == 0u);
        u32 active_width = (i == 0u) ? hactive : 0u;
        for (u32 j = 0; j < secondary_count; j++)
            if (secondary[j].window == i) {
                active_window = true;
                active_width = secondary[j].mode.hactive;
            }
        dpush1(&core, CA7D_WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(i),
               active_window ? 0xFu : 0u);   /* active scanout supports RGB packed 1/2/4/8 bpp */
        dpush1(&core, CA7D_WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(i), 0u);
        dpush1(&core, CA7D_WINDOW_SET_MAX_INPUT_SCALE_FACTOR(i),
               active_window ? (CA7D_SCALE_FACTOR_1X |
                                (CA7D_SCALE_FACTOR_1X << 16)) : 0u);
        dpush1(&core, CA7D_WINDOW_SET_WINDOW_USAGE_BOUNDS(i),
               CA7D_WINDOW_USAGE_BOUNDS_DEFAULT |
               (active_window ? ((active_width + 22u) & 0x7fffu) : 0u));
        dpush1(&core, CA7D_WINDOW_SET_CONTROL(i), i >> 1);                    /* fixed: wins 0/1->head0, ... */
        /* Clear mappings first.  SetMultiTileConfigCA assigns the RM-IMP-derived
         * physical-window masks after each complete head timing proposal. */
        dpush1(&core, CA7D_WINDOW_SET_PHYSICAL(i), 0u);
    }
    for (u32 i = 0; i < 4u; i++) {
        bool active_head = (i == 0u);
        for (u32 j = 0; j < secondary_count; j++)
            if (secondary[j].head == i) active_head = true;
        dpush1(&core, CA7D_HEAD_SET_HEAD_USAGE_BOUNDS(i),
               active_head ? CA7D_HEAD_USAGE_BOUNDS_VAL : 0u);
        /* As with physical-window mapping, cold tile masks are assigned only
         * after that head's complete timing/DSC state has been proposed. */
        dpush1(&core, CA7D_HEAD_SET_TILE_MASK(i), 0u);
        dpush1(&core, CA7D_TILE_SET_TILE_SIZE(i), 0u);
    }

    /* The single core update below contains mapping plus every head/SOR. */
    const u32 NTFY_SENTINEL = 0xDEADBEEFu;

    /* ---- core half: head mode, then SOR attach, then display id ----
     * Preserve nvEvoAttachConnector's exact method order.  It emits ORSetControl
     * first and HeadSetDisplayId second, after timing/output-resource programming.
     * The old Kestrel order put DISPLAY_ID before all timing methods and the SOR
     * method last, which does not match the working CA7D transaction. */
    /* Current 595.99.02 EvoSetRasterParams9 starts by explicitly resetting the
     * overscan colour, even when that value is zero. */
    dpush1(&core, CA7D_HEAD_SET_OVERSCAN_COLOR(0), 0u);
    dpush1(&core, CA7D_HEAD_SET_RASTER_SIZE(0), (htotal & 0xFFFFu) | (vtotal << 16));
    /* Raster sync/blank per nouveau head.c:299-306: synce=hsync_w-1,
     * blanke=htotal-hsync_start-1, blanks=blanke+active. */
    dpush1(&core, CA7D_HEAD_SET_RASTER_SYNC_END(0),
           ((hsync_end - hsync_start - 1u) & 0x7FFFu) | (((vsync_end - vsync_start - 1u) & 0x7FFFu) << 16));
    dpush1(&core, CA7D_HEAD_SET_RASTER_BLANK_END(0),
           ((htotal - hsync_start - 1u) & 0x7FFFu) | (((vtotal - vsync_start - 1u) & 0x7FFFu) << 16));
    dpush1(&core, CA7D_HEAD_SET_RASTER_BLANK_START(0),
           ((htotal - hsync_start - 1u + hactive) & 0x7FFFu) | (((vtotal - vsync_start - 1u + vactive) & 0x7FFFu) << 16));
    /* NVIDIA's EvoSetRasterParams3 always programs MIN_FRAME_IDLE before the
     * pixel clock.  The display-class error checker requires at least two
     * leading lines; omitting this method leaves reset state and can make UPDATE
     * fault/stall.  With a 1:1 viewport and no overscan, nvComputeMinFrameIdle()
     * reduces to:
     *   leading  = rasterBlankEnd.y + 1 = vtotal - vsync_start
     *   trailing = vtotal - leading - vactive = vsync_start - vactive
     * (clca7d.h:1342-1344, nvkms-evo3.c:nvComputeMinFrameIdle). */
    u32 min_idle_leading = vtotal - vsync_start;
    u32 min_idle_trailing = vsync_start - vactive;
    if (min_idle_leading < 2u) min_idle_leading = 2u;
    dpush1(&core, CA7D_HEAD_SET_MIN_FRAME_IDLE(0),
           (min_idle_leading & 0x7FFFu) | ((min_idle_trailing & 0x7FFFu) << 16));
    dpush1(&core, CA7D_HEAD_SET_CONTROL(0), CA7D_STRUCTURE_PROGRESSIVE);
    dpush1(&core, CA7D_SET_CONTROL, 0u);
    /* LOCK_CHAIN immediately after HEAD_SET_CONTROL, exactly as nvkms
     * EvoSetHeadControlC9 (nvkms-evo4.c:568-570).  POSITION=0: this head is not in a
     * raster-lock chain.  MANDATORY on ca7d and was MISSING - a supervisor handed an
     * incomplete head state can decline the SOR attach without raising an FE
     * exception, which matches "head activates but SOR never attaches". */
    dpush1(&core, CA7D_HEAD_SET_LOCK_CHAIN(0), 0u);
    u64 pclk_hz = (u64)m->pixel_clock_khz * 1000ull;
    dpush1(&core, CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(0), (u32)(pclk_hz & 0x7FFFFFFFull));
    dpush1(&core, CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI(0), (u32)((pclk_hz >> 31) & 0xFu));
    /* NOT_DRIVER=FALSE, HOPPING=DISABLE, HOPPING_MODE=VBLANK are all zero, but
     * the method itself is part of the mandatory NVKMS raster sequence. */
    dpush1(&core, CA7D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION(0), 0u);
    dpush1(&core, CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(0), (u32)(pclk_hz & 0x7FFFFFFFull));
    dpush1(&core, CA7D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI_MAX(0), (u32)((pclk_hz >> 31) & 0xFu));
    dpush1(&core, CA7D_HEAD_SET_FRAME_PACKED_VACTIVE_COLOR(0), 0u);
    dpush1(&core, CA7D_HEAD_SET_HDMI_CTRL(0), 0u);              /* normal/non-3D */
    /* New in the current binary wrapper around EvoSetRasterParams9.  Its helper
     * returns zero for this non-DSC SST mode, but the CA method is still emitted. */
    dpush1(&core, CA7D_HEAD_SET_RASTER_HBLANK_DELAY(0), 0u);
    /* No QSYNC/external VPLL reference: EvoSetHeadRefClkCA emits NO_PREF=0. */
    dpush1(&core, CA7D_HEAD_SET_SW_SPARE_A(0), 0u);
    kinfo("nv-disp", "  595 raster sequence: min-idle %u/%u pclk %llu Hz hi %#x, "
                     "zero reset methods 2078/207c/2080/2194/2364",
          min_idle_leading, min_idle_trailing, (unsigned long long)pclk_hz,
          (u32)((pclk_hz >> 31) & 0xFu));
    /* VIEWPORT_POINT_IN (origin) is REQUIRED with VIEWPORT_SIZE_IN and was MISSING;
     * nvkms pushes it in the viewport sequence (nvkms-evo4.c:734-736).  (0,0) scans
     * from the surface origin (X 14:0, Y 30:16 - clca7d.h:957-959). */
    dpush1(&core, CA7D_HEAD_SET_VIEWPORT_POINT_IN(0), 0u);
    dpush1(&core, CA7D_HEAD_SET_VIEWPORT_SIZE_IN(0), (hactive & 0x7FFFu) | (vactive << 16));
    dpush1(&core, CA7D_HEAD_SET_VIEWPORT_SIZE_OUT(0), (hactive & 0x7FFFu) | (vactive << 16));
    dpush1(&core, CA7D_HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(0), 0u);
    dpush1(&core, CA7D_HEAD_SET_CONTROL_OUTPUT_SCALER(0),
           CA7D_OUTPUT_SCALER_TAPS_2);
    dpush1(&core, CA7D_HEAD_SET_MAX_OUTPUT_SCALE_FACTOR(0),
           CA7D_SCALE_FACTOR_1X | (CA7D_SCALE_FACTOR_1X << 16));
    /* OUTPUT_RESOURCE: CRC_MODE(1:0)=COMPLETE_RASTER(1) | HSYNC_POLARITY(bit2) |
     * VSYNC_POLARITY(bit3) | PIXEL_DEPTH(7:4)=BPP_24_444.  1=NEGATIVE on ca7d
     * (clca7d.h:650-655).  Polarity from the EDID first-DTD flags byte (abs offset
     * 0x47; digital separate sync when bits4:3==0b11): NVKMS hSyncPol = !(b17&0x02),
     * vSyncPol = !(b17&0x04) (Opus MSA trace; drm_edid.c:3604).  Our hardcoded 0/0
     * made VSYNC positive - wrong for this CVT-RB panel (V must be NEGATIVE). */
    u32 out_res = 1u | (CA7D_PIXEL_DEPTH_BPP_24_444 << 4)
                | CA7D_OUT_RES_EXT_PACKET_WIN_NONE;   /* EXT_PACKET_WIN=NONE, was MISSING (nvkms-evo4.c:599) */
    {
        u8 b17 = m->flags;
        if ((b17 & 0x18u) == 0x18u) {                 /* digital separate sync */
            if (!(b17 & 0x02u)) out_res |= (1u << 2); /* HSYNC negative */
            if (!(b17 & 0x04u)) out_res |= (1u << 3); /* VSYNC negative */
            kinfo("nv-disp", "  sync polarity from EDID DTD b17=%#x: hsync %s vsync %s",
                  b17, (out_res & 4) ? "NEG" : "POS", (out_res & 8) ? "NEG" : "POS");
        }
    }
    dpush1(&core, CA7D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(0), out_res);
    dpush1(&core, CA7D_HEAD_SET_PROCAMP(0), CA7D_PROCAMP_RGB_VESA);
    dpush1(&core, CA7D_HEAD_SET_DITHER_CONTROL(0), 0);
    if (primary_plan.enabled) {
        u32 ctl = CA7D_DSC_CONTROL_ENABLE |
                  CA7D_DSC_CONTROL_FULL_ICH_PRECISION |
                  CA7D_DSC_CONTROL_FORCE_ICH_RESET |
                  (2u << CA7D_DSC_CONTROL_FLATNESS_SHIFT);
        dpush1(&core, CA7D_HEAD_SET_DSC_CONTROL(0), ctl);
        dpush1(&core, CA7D_HEAD_SET_DSC_PPS_CONTROL(0),
               CA7D_DSC_PPS_ENABLE | CA7D_DSC_PPS_LOCATION_VSYNC |
               CA7D_DSC_PPS_SIZE_128_BYTES);
        for (u32 i = 0; i < DSC_MAX_PPS_SIZE_DWORD; i++)
            dpush1(&core, CA7D_HEAD_SET_DSC_PPS_DATA0(0) + i * 4u,
                   primary_plan.pps[i]);
        dpush1(&core, CA7D_HEAD_SET_DSC_PPS_HEAD(0), 0x007f1000u);
    } else {
        dpush1(&core, CA7D_HEAD_SET_DSC_CONTROL(0), 0u);
        dpush1(&core, CA7D_HEAD_SET_DSC_PPS_CONTROL(0), 0u);
    }
    /* Consume RM-IMP's required first tiling assignment through the same
     * tile/physical-window/size sequence as EvoSetMultiTileConfigCA. */
    disp_push_multitile(&core, 0u, 0u, hactive,
                        g_imp_tile_mask[0], g_imp_phywin_mask[0],
                        g_imp_dsc_slices[0] != 0u, g_imp_dsc_slices[0]);
    disp_push_dp_vsc_rgb8(&core, 0u);
    /* OLUT (output LUT) - MANDATORY on ca7d; without it the head outputs black
     * (headca7d.c:186-215).  Point head 0 at the pre-filled identity LUT. */
    if (g_olut_ok) {
        dpush1(&core, CA7D_HEAD_SET_SURFACE_ADDRESS_HI_OLUT(0), (u32)(g_olut_fb >> 32));
        dpush1(&core, CA7D_HEAD_SET_SURFACE_ADDRESS_LO_OLUT(0),
               ((u32)g_olut_fb & 0xFFFFFFF0u) | (CA7E_LO_ISO_TARGET_PHYSICAL_NVM << 2) | 1u);
        dpush1(&core, CA7D_HEAD_SET_OLUT_CONTROL(0), CA7D_OLUT_CONTROL_VAL);
        dpush1(&core, CA7D_HEAD_SET_OLUT_FP_NORM_SCALE(0), 0xFFFFFFFFu);
    }
    g_relight.sor_attach_value =
        (1u << CA7D_SOR_OWNER_MASK_SHIFT) |
        (sor_protocol << CA7D_SOR_PROTOCOL_SHIFT);
    dpush1(&core, CA7D_SOR_SET_CONTROL(sor), g_relight.sor_attach_value);
    dpush1(&core, CA7D_HEAD_SET_DISPLAY_ID(0, 0), display_id);
    kinfo("nv-disp", "  SOR%u attach method: value %#x (owner head0 %#x, protocol %#x)",
          sor, g_relight.sor_attach_value,
          g_relight.sor_attach_value & 0xFFu,
          (g_relight.sor_attach_value >> 8) & 0xFu);

    /* Queue every secondary head and connector into this same core update.
     * This is ApplyProposedModeSetStateOneApiHeadPreUpdate() for the remaining
     * heads, before KickoffModesetUpdateState() is called once. */
    for (u32 j = 0; j < secondary_count; j++) {
        disp_secondary_plan_t *sp = &secondary[j];
        disp_push_head_mode(&core, sp->head, sp->window, sp->sor, sp->protocol,
                            sp->sink->display_id, &sp->mode,
                            sp->is_hdmi ? NULL : &sp->dp_plan,
                            sp->is_hdmi ? &sp->hdmi_plan : NULL);
    }

    /* KickoffModesetUpdateState() runs DPLib's pre-modeset hook here: link
     * training/configuration has completed, every core method is staged, but
     * CA7D_UPDATE has not yet been kicked.  Cache the normal RGB MSA override
     * for each affected DP SST connector in precisely that window. */
    if (sor_protocol == OR_PROTOCOL_SOR_DP_A ||
        sor_protocol == OR_PROTOCOL_SOR_DP_B)
        disp_dp_pre_modeset_msa(c, rm, objcom, display_id, "primary");
    for (u32 j = 0; j < secondary_count; j++) {
        disp_secondary_plan_t *sp = &secondary[j];
        if (!sp->is_hdmi)
            disp_dp_pre_modeset_msa(c, rm, objcom,
                                    sp->sink->display_id, "secondary");
    }
    /* Arm the core UPDATE-completion notifier on THIS (single) core UPDATE
     * (coreca7d_update, coreca7d.c:16-56): MODE_WRITE | NOTIFY_ENABLE, pointed at the
     * pre-allocated notifier surface.  The display engine overwrites the sentinel when
     * the interlocked promotion completes.  Notifier methods are pushed BEFORE the
     * UPDATE; NOTIFY_DISABLE is pushed after. */
    if (g_notifier_ok) {
        dpush1(&core, CA7D_SET_SURFACE_ADDRESS_HI_NOTIFIER, (u32)(g_notifier_fb >> 32));
        dpush1(&core, CA7D_SET_SURFACE_ADDRESS_LO_NOTIFIER,
               ((u32)g_notifier_fb & 0xFFFFFFF0u) | (CA7E_LO_ISO_TARGET_PHYSICAL_NVM << 2) | 1u);
        dpush1(&core, CA7D_SET_NOTIFIER_CONTROL, CA7D_NOTIFIER_CONTROL_NOTIFY_ENABLE); /* MODE_WRITE(0)|NOTIFY_ENABLE */
    }
    /* windowMappingChanged=true: no windows may be interlocked with the core
     * update that assigns them.  This is NVKMS noCoreInterlockMask in hardware. */
    dpush1(&core, CA7D_SET_INTERLOCK_FLAGS, 0u);
    dpush1(&core, CA7D_SET_WINDOW_INTERLOCK_FLAGS, 0u);
    dpush1(&core, CA7D_UPDATE, 1u /* RELEASE_ELV_TRUE */);
    if (g_notifier_ok)
        dpush1(&core, CA7D_SET_NOTIFIER_CONTROL, 0u);      /* NOTIFY_DISABLE */

    /* Prepare the decoupled flips now; they are not kicked until the core
     * notifier confirms that ownership and every head/SOR have promoted. */
    dpush1(&win, CA7E_SET_SIZE, (hactive & 0xFFFFu) | (vactive << 16));
    dpush1(&win, CA7E_SET_STORAGE, 0);                         /* BLOCK_HEIGHT=0 (pitch) */
    dpush1(&win, CA7E_SET_PARAMS, fmt);
    dpush1(&win, CA7E_SET_PLANAR_STORAGE(0), (u32)((surf_pitch >> 6) & 0x1FFFu));
    {
        /* (C) Scan out the KNOWN-good red VRAM surface (VRAM-local FB offset,
         * TARGET_PHYSICAL_NVM per clca7e.h CA7E_LO_ISO_TARGET_PHYSICAL_NVM); the
         * fallback boot-fb path is retained only if the VRAM alloc failed. */
        u64 fb = scanout_fb;
        dpush1(&win, CA7E_SET_SURFACE_ADDRESS_HI_ISO(0), (u32)(fb >> 32));
        u32 lo = ((u32)fb & 0xFFFFFFF0u)                       /* ADDRESS_LO 31:4 (16B aligned) */
               | (CA7E_LO_ISO_TARGET_PHYSICAL_NVM << 2)        /* VRAM-local; boot-fb fallback reuses NVM as a best-effort guess */
               | (CA7E_LO_ISO_KIND_PITCH << 1)
               | CA7E_LO_ISO_ENABLE;
        dpush1(&win, CA7E_SET_SURFACE_ADDRESS_LO_ISO(0), lo);
        kinfo("nv-disp", "  SET_SURFACE_ADDRESS: fb %#llx target=PHYSICAL_NVM(%u) kind=PITCH lo=%#x (%s)",
              (unsigned long long)fb, CA7E_LO_ISO_TARGET_PHYSICAL_NVM, lo,
              scanout_vram ? "VRAM red surface" : "boot fb fallback");
    }
    dpush1(&win, CA7E_SET_POINT_IN(0), 0);
    dpush1(&win, CA7E_SET_SIZE_IN, (hactive & 0xFFFFu) | (vactive << 16));
    dpush1(&win, CA7E_SET_SIZE_OUT, (hactive & 0xFFFFu) | (vactive << 16));
    dpush1(&win, CA7E_SET_PRESENT_CONTROL, 0);                 /* NON_TEARING, interval 0 */
    dpush1(&win, CA7E_SET_INTERLOCK_FLAGS, 0u);
    dpush1(&win, CA7E_SET_WINDOW_INTERLOCK_FLAGS, 0u);
    dpush1(&win, CA7E_SW_SET_MCLK_SWITCH, 0u);
    dpush1(&win, CA7E_UPDATE, 1u);
    dpush1(&win, CA7E_SW_SET_MCLK_SWITCH, 1u);

    for (u32 j = 0; j < secondary_count; j++)
        disp_push_window_surface(&secondary[j].win, secondary[j].window, fmt,
                                 surf_pitch, scanout_fb, &secondary[j].mode);

    /* First promote the complete core topology.  Window kickoffs follow only
     * after the synchronous core completion below. */
    if (g_notifier_ok) {
        u32 cw = 0xFFFFFFFFu;
        for (u32 o = 0; o < 16u; o += 4u) disp_vram_wr32(c, g_notifier_fb + o, NTFY_SENTINEL, &cw);
    }
    dkick(c, &core);

    g_relight.olut_ok = g_olut_ok;

    /* Poll the notifier for the UPDATE-completion write (up to ~200ms). */
    if (g_notifier_ok) {
        u32 nv = NTFY_SENTINEL;
        for (int t = 0; t < 200; t++) {
            nv = disp_vram_rd32(c, g_notifier_fb);
            if (nv != NTFY_SENTINEL) break;
            u64 dl = g_uptime_ms + 1; while (g_uptime_ms < dl) timer_udelay(500);
        }
        g_relight.update_notified = (nv != NTFY_SENTINEL);
        g_relight.notifier_val = nv;
        kinfo("nv-disp", "core UPDATE notifier: %#x -> %s", nv,
              g_relight.update_notified ? "LATCHED (update completed)"
                                        : "NOT written (update did not complete)");
    }

    /* The notifier is the promotion proof; GET is also required so a missing
     * notifier allocation cannot let window flips race the core methods. */
    {
        u32 put = core.at << 2, get = 0u;
        for (u32 t = 0; t < 100u; t++) {
            get = nv_rd32(c, CORE_USER_BASE + 4u);
            if (get == put) break;
            u64 dl = g_uptime_ms + 2u;
            while (g_uptime_ms < dl) timer_udelay(500);
        }
        if (get != put) {
            kerr("nv-disp", "atomic core topology was not consumed (%#x/%#x); refusing window flips",
                 get, put);
            goto done;
        }
    }

    /* ApplyProposedModeSetStateOneDispFlip() after the completed core update. */
    dkick(c, &win);
    for (u32 j = 0; j < secondary_count; j++)
        dkick(c, &secondary[j].win);

    /* Probe the SOR attach state right after the single interlocked attach: did the
     * head+SOR update bind the SOR to head0 (owner mask nonzero, proto DP=8)?  With
     * the attach now interlocked with the live window, the supervisor's
     * nv50_disp_super_ior_asy() should find a head to bind (nvEvoAttachConnector,
     * nvkms-evo.c:397-413).  0x100 (TMDS/no-head) = SOR still not bound. */
    {
        g_relight.sor_owner_assy_p1 = nv_rd32(c, PDISP_SOR_OWNER_ASSY(g_relight.or_index));
        g_relight.sor_owner_arm_p1  = nv_rd32(c, PDISP_SOR_OWNER_ARM(g_relight.or_index));
        g_relight.xbar_a_after = nv_rd32(c, PDISP_XBAR_LINK_A(xbar_output));
        g_relight.xbar_b_after = nv_rd32(c, PDISP_XBAR_LINK_B(xbar_output));
        kinfo("nv-disp", "  post-attach SOR%u state: ASSY %#x ARM %#x (%s)",
              g_relight.or_index, g_relight.sor_owner_assy_p1, g_relight.sor_owner_arm_p1,
              (g_relight.sor_owner_arm_p1 & 0xFFu) ? "attached to head" : "still detached");
        kinfo("nv-disp", "  connector XBAR out%u post-attach: A %#x B %#x (expected low5 %#x)",
              xbar_output, g_relight.xbar_a_after, g_relight.xbar_b_after,
              g_relight.xbar_expected);
    }

    /* Give at least one frame interval to elapse, then sample direct raster
     * progress.  These are read-only observations; do not clear any status. */
    {
        u64 dl = g_uptime_ms + 25u;
        while (g_uptime_ms < dl) timer_udelay(500);
    }
    g_relight.rg_loadv_after = nv_rd32(c, PDISP_RG_IN_LOADV_COUNTER(0));
    g_relight.postcomp_loadv_after = nv_rd32(c, PDISP_POSTCOMP_LOADV_COUNTER(0));
    g_relight.crashlock_after = nv_rd32(c, PDISP_RG_CRASHLOCK_COUNTER(0));
    kinfo("nv-disp", "raster counters: RG LOADV %#x->%#x POSTCOMP %#x->%#x "
                     "CRASHLOCK %#x->%#x (V %#x->%#x)",
          g_relight.rg_loadv_before, g_relight.rg_loadv_after,
          g_relight.postcomp_loadv_before, g_relight.postcomp_loadv_after,
          g_relight.crashlock_before, g_relight.crashlock_after,
          g_relight.crashlock_before >> 16, g_relight.crashlock_after >> 16);

    /* SOR pad state after UPDATE: the raster (LOADV) can advance while the SOR
     * pad stays off - that is exactly the "No Input despite scanning" blocker.
     * These are direct BAR0 reads of the SOR the GSP-RM assigned (or_index),
     * using the verified offsets nv_dp.c drives (nv.h:263-264).  If the
     * DISPLAY_CHANGE bracket made GSP finalize the pad, LINKCTL enable + PADCTL
     * lane bits are set; all-zero here = SOR still not transmitting. */
    {
        u32 soff = g_relight.or_index * NV_PDISP_SOR_STRIDE;
        g_relight.sor_dp_linkctl = nv_rd32(c, NV_PDISP_SOR_DP_LINKCTL + soff); /* link 0 */
        g_relight.sor_dp_padctl  = nv_rd32(c, NV_PDISP_SOR_DP_PADCTL + soff);
        kinfo("nv-disp", "SOR%u pad state: DP_LINKCTL %#x DP_PADCTL %#x",
              g_relight.or_index, g_relight.sor_dp_linkctl, g_relight.sor_dp_padctl);
    }

    /* ---- ARM-vs-ASSY promotion probe (the decisive "did the UPDATE promote"
     *      test; nouveau gv100.c:183-296).  All direct BAR0 reads, no writes.
     * ASSY = what we pushed into the assembly; ARM = what the display engine
     * actually latched/promoted.  If ASSY holds our 2560x1440 timing but ARM is
     * zero/stale, the core UPDATE was accepted but NEVER promoted - which is the
     * whole "consumed, no exception, but head never activates" symptom.  We also
     * sample the SOR owner/power/seq and the core-channel + supervisor state so
     * the log names the exact stage that failed instead of us inferring it. */
    {
        u32 h = 0u;                              /* head 0 */
        u32 s = g_relight.or_index;              /* the assigned SOR */
        g_relight.head_raster_assy = nv_rd32(c, PDISP_HEAD_RASTER_ASSY(h));
        g_relight.head_raster_arm  = nv_rd32(c, PDISP_HEAD_RASTER_ARM(h));
        g_relight.head_pclk_arm    = nv_rd32(c, PDISP_HEAD_PCLK_ARM(h));
        g_relight.rg_dpca_1        = nv_rd32(c, PDISP_RG_DPCA(h));
        { u64 dl = g_uptime_ms + 5u; while (g_uptime_ms < dl) timer_udelay(500); }
        g_relight.rg_dpca_2        = nv_rd32(c, PDISP_RG_DPCA(h));
        g_relight.sf_dp_ctl        = nv_rd32(c, PDISP_SF_DP_CTL(h));
        g_relight.sor_owner_assy   = nv_rd32(c, PDISP_SOR_OWNER_ASSY(s));
        g_relight.sor_owner_arm    = nv_rd32(c, PDISP_SOR_OWNER_ARM(s));
        g_relight.sor_pwr          = nv_rd32(c, PDISP_SOR_PWR(s));
        g_relight.sor_seq_ctl      = nv_rd32(c, PDISP_SOR_SEQ_CTL(s));
        g_relight.core_chan_state  = nv_rd32(c, PDISP_FE_CORE_CHAN_STATE);
        g_relight.sv_pending       = nv_rd32(c, PDISP_FE_SV_INTR);
        u32 requested_raster = (htotal & 0xFFFFu) | (vtotal << 16);
        u32 expected_raster = requested_raster;
        bool assy_has_requested_mode = g_relight.head_raster_assy == expected_raster;
        bool arm_has_requested_mode = g_relight.head_raster_arm == expected_raster;
        kinfo("nv-disp", "PROMOTE probe: head0 raster expected %#x ASSY %#x ARM %#x (%s) pclkARM %#x",
              expected_raster,
              g_relight.head_raster_assy, g_relight.head_raster_arm,
              arm_has_requested_mode ? "REQUESTED MODE PROMOTED" :
              (assy_has_requested_mode ? "requested ASSY not promoted" :
                                         "requested methods not assembled"),
              g_relight.head_pclk_arm);
        kinfo("nv-disp", "  RG_DPCA %#x -> %#x (%s), SF_DP_CTL %#x (active-sym %s wm %u)",
              g_relight.rg_dpca_1, g_relight.rg_dpca_2,
              (g_relight.rg_dpca_1 != g_relight.rg_dpca_2) ? "scanning" : "frozen",
              g_relight.sf_dp_ctl, (g_relight.sf_dp_ctl & (1u << 27)) ? "ON" : "off",
              g_relight.sf_dp_ctl & 0x3Fu);
        kinfo("nv-disp", "  SOR%u owner ASSY %#x ARM %#x (%s) PWR %#x (%s) SEQ %#x (%s)",
              s, g_relight.sor_owner_assy, g_relight.sor_owner_arm,
              (g_relight.sor_owner_arm & 0xFFu) ? "bound" : "detached",
              g_relight.sor_pwr,
              ((g_relight.sor_pwr & 1u) && !(g_relight.sor_pwr & 0x80000000u)) ? "PU-normal" : "not-up",
              g_relight.sor_seq_ctl, (g_relight.sor_seq_ctl & (1u << 28)) ? "busy" : "settled");
        kinfo("nv-disp", "  core-chan state %#x (%s), supervisor pending %#x",
              g_relight.core_chan_state,
              (((g_relight.core_chan_state >> 16) & 0x1Fu) == 0xBu) ? "quiescent/good" : "NOT idle",
              g_relight.sv_pending & 0x7u);
    }

    /* Read (do not acknowledge/clear) the NVDisplay front-end exception state.
     * Volta+ maps core to CHID 0 and window0 to CHID 1.  Each FE_EXCEPT slot is
     * STAT/DATA/CODE at 0x611020 + chid*12; STAT bits 11:0 are method/4 and
     * bits 14:12 are the exception reason.  These registers are unchanged in
     * nouveau's GV100/TU102/GA102 display path and give us the rejected method
     * when an UPDATE was consumed but its completion notifier never fires. */
    g_relight.core_exc_stat = nv_rd32(c, 0x00611020u);
    g_relight.core_exc_data = nv_rd32(c, 0x00611024u);
    g_relight.core_exc_code = nv_rd32(c, 0x00611028u);
    g_relight.win_exc_stat  = nv_rd32(c, 0x0061102Cu);
    g_relight.win_exc_data  = nv_rd32(c, 0x00611030u);
    g_relight.win_exc_code  = nv_rd32(c, 0x00611034u);
    g_relight.exc_other     = nv_rd32(c, 0x00611854u);
    g_relight.exc_window    = nv_rd32(c, 0x0061184Cu);
    g_relight.ctrl_detail   = nv_rd32(c, 0x00611848u);
    g_relight.awaken_win    = nv_rd32(c, 0x00611858u);
    g_relight.awaken_other  = nv_rd32(c, 0x0061185Cu);
    g_relight.sem_win       = nv_rd32(c, 0x00611868u);
    g_relight.ctrl_intr     = nv_rd32(c, 0x00611C30u);
    if (g_relight.exc_other & 1u)
        kinfo("nv-disp", "FE ACTIVE core exception: stat/data/code %#x/%#x/%#x "
                         "(reason %u method %#x)",
              g_relight.core_exc_stat, g_relight.core_exc_data, g_relight.core_exc_code,
              (g_relight.core_exc_stat >> 12) & 7u,
              (g_relight.core_exc_stat & 0xFFFu) << 2);
    else
        kinfo("nv-disp", "FE core summary clear; slot raw %#x/%#x/%#x is stale/inactive",
              g_relight.core_exc_stat, g_relight.core_exc_data, g_relight.core_exc_code);
    if (g_relight.exc_window & 1u)
        kinfo("nv-disp", "FE ACTIVE win0 exception: stat/data/code %#x/%#x/%#x "
                         "(reason %u method %#x)",
              g_relight.win_exc_stat, g_relight.win_exc_data, g_relight.win_exc_code,
              (g_relight.win_exc_stat >> 12) & 7u,
              (g_relight.win_exc_stat & 0xFFFu) << 2);
    else
        kinfo("nv-disp", "FE win0 summary clear; slot raw %#x/%#x/%#x is stale/inactive",
              g_relight.win_exc_stat, g_relight.win_exc_data, g_relight.win_exc_code);
    kinfo("nv-disp", "FE raw: summary other/win %#x/%#x ctrl-detail %#x awaken win/other "
                     "%#x/%#x sem-win %#x ctrl-intr %#x",
          g_relight.exc_other, g_relight.exc_window, g_relight.ctrl_detail,
          g_relight.awaken_win, g_relight.awaken_other, g_relight.sem_win,
          g_relight.ctrl_intr);

    /* Confirm both channels consumed their pushbuffers.  In an interlocked attach,
     * a core GET==PUT alone cannot prove the window partner reached its UPDATE. */
    {
        u32 core_put = core.at << 2;
        u32 core_get = 0;
        for (int t = 0; t < 40; t++) {
            core_get = nv_rd32(c, CORE_USER_BASE + 0x4u);
            if (core_get == core_put) break;
            u64 dl = g_uptime_ms + 5; while (g_uptime_ms < dl) timer_udelay(500);
        }
        g_relight.core_put = core_put;
        g_relight.core_get = core_get;
        kinfo("nv-disp", "core channel GET %#x / PUT %#x -> %s", core_get, core_put,
              core_get == core_put ? "CONSUMED (channel processed the modeset)"
                                   : "STUCK (channel did NOT consume - no scanout change)");

        u32 win_put = win.at << 2;
        u32 win_get = 0;
        for (int t = 0; t < 40; t++) {
            win_get = nv_rd32(c, WINDOW_USER_BASE(0) + 0x4u);
            if (win_get == win_put) break;
            u64 dl = g_uptime_ms + 5; while (g_uptime_ms < dl) timer_udelay(500);
        }
        g_relight.window_put = win_put;
        g_relight.window_get = win_get;
        kinfo("nv-disp", "window0 channel GET %#x / PUT %#x -> %s", win_get, win_put,
              win_get == win_put ? "CONSUMED (decoupled flip complete)"
                                 : "STUCK (surface flip not consumed)");
        for (u32 j = 0; j < secondary_count; j++) {
            disp_secondary_plan_t *sp = &secondary[j];
            u32 sput = sp->win.at << 2, sget = 0u;
            for (u32 t = 0; t < 100u; t++) {
                sget = nv_rd32(c, WINDOW_USER_BASE(sp->window) + 4u);
                if (sget == sput) break;
                u64 dl = g_uptime_ms + 2u;
                while (g_uptime_ms < dl) timer_udelay(500);
            }
            kinfo("nv-disp", "window%u channel GET %#x / PUT %#x -> %s",
                  sp->window, sget, sput,
                  sget == sput ? "CONSUMED (decoupled flip complete)" :
                                 "STUCK (surface flip not consumed)");
        }
    }

    /* Keep the one full-topology DISPLAY_CHANGE bracket open while all
     * secondary heads are programmed below. */
    g_relight.lit_mask |= display_id;

    /* Every secondary was already included in the single core transaction and
     * its decoupled window flip above.  Do not issue per-connector updates here. */
    if (scanout_vram) {
        /* Match nvRmBeginEndModeset: END carries the same complete proposed
         * display mask as START, after every head update has been kicked. */
        {
            sys_display_change_t dc; memset(&dc, 0, sizeof dc);
            dc.newDevices = modeset_change_mask;
            dc.enable = NV0073_DISPLAY_CHANGE_END;
            disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE,
                      &dc, sizeof dc, "DISPLAY_CHANGE END (full topology)");
            modeset_change_open = false;
        }

        /* The boot/GOP display is DP in this path.  Validate it with the same
         * owner-plus-receiver rule as cold secondaries so a stale ARM value or
         * a link that drops after the core promotion cannot masquerade as a
         * valid video signal. */
        {
            u32 arm = 0u, stable = 0u;
            u8 ls[3] = {0, 0, 0};
            for (u32 t = 0; t < 100u && stable < 2u; t++) {
                arm = nv_rd32(c, PDISP_SOR_OWNER_ARM(g_relight.or_index));
                bool owner = (arm & 1u) != 0u;
                bool transport = disp_dp_sink_link_ok(c, rm, objcom,
                                                       display_id, lanes, ls);
                stable = owner && transport ? stable + 1u : 0u;
                if (stable < 2u) {
                    u64 dl = g_uptime_ms + 10u;
                    while (g_uptime_ms < dl) timer_udelay(500);
                }
            }
            if (stable >= 2u) {
                g_relight.lit_mask |= display_id;
                kinfo("nv-disp", "primary display %#x post-END: SOR%u ARM %#x, link %02x/%02x/%02x -> VALID VIDEO",
                      display_id, g_relight.or_index, arm,
                      ls[0], ls[1], ls[2]);
            } else {
                g_relight.lit_mask &= ~display_id;
                kwarn("nv-disp", "primary display %#x post-END: SOR%u ARM %#x, link %02x/%02x/%02x -> NO VALID VIDEO after 1000ms",
                      display_id, g_relight.or_index, arm,
                      ls[0], ls[1], ls[2]);
            }
        }

        /* END is asynchronous with respect to SOR power-up and sink clock
         * recovery.  The old one-shot ARM read ran immediately after END and
         * could start the test before a monitor acquired the stream.  Poll for
         * up to one second; for DP require two consecutive valid receiver-link
         * samples as well as the correct SOR owner. */
        for (u32 j = 0; j < secondary_count; j++) {
            disp_secondary_plan_t *sp = &secondary[j];
            disp_sink_t *sink = sp->sink;
            u32 arm = 0u, stable = 0u;
            u8 ls[3] = {0, 0, 0};
            for (u32 t = 0; t < 100u && stable < 2u; t++) {
                arm = nv_rd32(c, PDISP_SOR_OWNER_ARM(sink->assigned_sor));
                bool owner = (arm & (1u << sink->assigned_head)) != 0u;
                bool transport = sp->is_hdmi ||
                    disp_dp_sink_link_ok(c, rm, objcom, sink->display_id,
                                         sp->link_lanes, ls);
                stable = owner && transport ? stable + 1u : 0u;
                if (stable < 2u) {
                    u64 dl = g_uptime_ms + 10u;
                    while (g_uptime_ms < dl) timer_udelay(500);
                }
            }
            if (stable >= 2u) {
                g_relight.lit_mask |= sink->display_id;
                kinfo("nv-disp", "secondary display %#x post-END: SOR%u ARM %#x, link %02x/%02x/%02x -> VALID VIDEO on head%u",
                      sink->display_id, sink->assigned_sor, arm,
                      ls[0], ls[1], ls[2], sink->assigned_head);
            } else {
                g_relight.lit_mask &= ~sink->display_id;
                kwarn("nv-disp", "secondary display %#x post-END: SOR%u ARM %#x, link %02x/%02x/%02x -> NO VALID VIDEO after 1000ms",
                      sink->display_id, sink->assigned_sor, arm,
                      ls[0], ls[1], ls[2]);
            }
        }

        /* Once the stream has passed the receiver gate, leave a fixed acquire
         * interval for the monitor's input detector/PLL before changing the
         * shared scanout surface for the first pattern. */
        {
            u64 dl = g_uptime_ms + 250u;
            while (g_uptime_ms < dl) timer_udelay(500);
        }

        /* RM's active view is meaningful only after the whole transaction. */
        {
            sys_get_active_t active; memset(&active, 0, sizeof active);
            active.head = 0u;
            g_relight.rm_active_ok =
                disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE,
                          &active, sizeof active, "SYSTEM_GET_ACTIVE head0");
            g_relight.rm_active_display = active.displayId;
            kinfo("nv-disp", "  RM head0 active displayId %#x (%s)", active.displayId,
                  active.displayId == display_id ? "matches requested output" :
                                                   "NOT routed to requested output");
        }
        g_relight.committed = true;

        /* GPU execution objects belong to the main RM client.  Restore it
         * BEFORE scanout VMM flushes, work-submit-token controls and patterns.
         * The former done-only restore caused status 87, token 0, and a stalled
         * copy GP_GET immediately after the modeset. */
        rm->client = saved_client;

        /* Bind before painting: this expanded suite must be generated by the
         * card's copy engine into the actual scanned-out allocation. */
        if (nv_chan_bind_scanout(scanout_fb, surf_bytes, (u32)surf_pitch,
                                 (u32)(surf_pitch / 4u),
                                 (u32)(surf_bytes / surf_pitch))) {
            g_relight.rgbw_mask = g_relight.lit_mask;
            if (cmdline_has("rgbtest") &&
                !disp_run_gpu_pattern_suite((u32)(surf_pitch / 4u),
                                            (u32)(surf_bytes / surf_pitch),
                                            g_relight.lit_mask))
                kwarn("nv-disp", "one or more GPU pattern submissions failed");
        } else {
            kwarn("nv-disp", "scanout committed but could not be bound to runtime channels; GPU pattern suite skipped");
        }
    } else {
        kwarn("nv-disp", "multi-output RGBW skipped: no verified VRAM-local scanout surface");
    }
    g_relight.stopped_at = "modeset committed";
    kinfo("nv-disp", "full maximum-capability modeset committed on a trained link - "
                     "framebuffer is scanned out (window %ux%u fmt %#x, %s)",
          hactive, vactive, fmt,
          primary_plan.enabled ? "DP DSC+FEC" : "DP uncompressed");

done:
    if (modeset_change_open) {
        sys_display_change_t dc; memset(&dc, 0, sizeof dc);
        dc.newDevices = modeset_change_mask ? modeset_change_mask : display_id;
        dc.enable = NV0073_DISPLAY_CHANGE_END;
        disp_ctrl(c, rm, objcom, NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE,
                  &dc, sizeof dc, "DISPLAY_CHANGE END (abort cleanup)");
    }
    /* Restore the main RM client so the compute/NVDEC/3D self-tests that run
     * after the re-light address their own object tree, not the disp client. */
    rm->client = saved_client;
}
