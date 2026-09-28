/* nv_chan.c - open real GPFIFO channels on Blackwell GB20x through GSP-RM, one
 * per engine, and prove the copy engine end to end.
 *
 * This is the gate for every kind of engine work (2D/copy, 3D, compute, video):
 * nothing draws on the GPU until a channel exists.  Built on the RM object tree
 * that nv_gsp_rm.c brings up over the live GSP-RM rings, and on nv_vmm.c for the
 * GPU page tables.  The recipe (class numbers, param structs, ordering) is from
 * the NVIDIA and nouveau references, including fifo.c and vmm.c in nouveau's
 * gsp/rm/r535 implementation.
 *
 * Each engine gets its OWN channel: a GPFIFO channel binds to a single engine
 * at creation, so 2D/copy, 3D/graphics, compute and the two video engines are
 * separate channels sharing the same recipe.  A channel's VA space is its own
 * (separate page directory), so the GPU virtual addresses can repeat between
 * channels; only the RM object HANDLES must be globally unique, so they carry a
 * per-channel index.
 *
 * Memory model per channel: one contiguous sysmem pool holds the VMM page
 * tables, the GPFIFO ring, the channel's USERD, the pushbuffer and a completion
 * semaphore.  The page directory is handed to RM as an EXTERNALLY-OWNED VA space
 * (SET_PAGE_DIRECTORY), so the card walks OUR tables; the ring/pushbuffer are
 * mapped into that VA space at fixed GPU virtual addresses.  Everything the CPU
 * has to poke after setup (GPPut in USERD, the pushbuffer, the semaphore) is
 * plain sysmem it can write directly - no BAR1 window needed for a first
 * bring-up.
 */
#include "kernel.h"
#include "mm.h"
#include "time.h"
#include "klog.h"
#include "nv.h"

#ifndef NV_CHAN_HOST_TEST

#include "nv_qmd.h"                    /* the compute launch descriptor        */
#include "../tools/shader_gpunop.h"    /* real sm_120 NOP (execution proof)    */
#include "../tools/shader_writeval.h"  /* real sm_120 store (data proof)       */
#include "../tools/shader_tri_raster.h" /* real sm_120 triangle rasterizer (3D) */
#include "../tools/shader_gui_raster.h" /* ordered native widget/composite SM */
#include "../tools/shader_gl_raster.h"  /* fixed-function application triangles */
#include "../tools/shader_shader_vm.h" /* bounded programmable vertex/fragment VM */
#include "../tools/shader_shader_raster.h" /* ordered programmable pixels */
#include "../tools/shader_shader_raster_fast.h" /* register-resident fragment subset */
#include "../tools/shader_shader_setup.h" /* GPU triangle assembly and clipping */
#include "shader_setup_compact_sass.h" /* trusted stable GPU command compaction */
#include "shader_setup_compact_small_sass.h" /* retained tiny-draw scan */
#include "../tools/shader_gui_present.h" /* native GPU viewport presentation */
#include "nv_surface.h"
#include "proc.h"
#include "nvrm_completion.h"
#include "nvdec_drv_h264.h"            /* NVDEC H.264 pic struct + methods     */
#include "nvdec_h264_context.h"        /* bounded real SPS/PPS/IDR parameters  */
#include "nvdec_h264_job.h"
#include "nv_video_layout.h"
#include "nv_video.h"
#include "nvenc_drv_h264.h"            /* NVIDIA-published NVENC firmware ABI  */
#include "nvenc_h264_policy.h"         /* driver-backed normal quantization policy */
#include "nvenc_h264_buffers.h"        /* checked CFB7 work-buffer requirements */
#include "nvenc_h264_stream.h"         /* exact-context SPS/PPS, actual IDR bytes */
#include "../tools/h264_iframe_test.h" /* structurally-validated 16x16 I_PCM IDR */

static u32 g_render_transaction;
static proc_t *g_render_pin;
bool nv_render_try_begin(void) {
    proc_t *pin = NULL;
    if (!proc_resource_pin(&pin)) return false;
    if (__atomic_exchange_n(&g_render_transaction, 1u, __ATOMIC_ACQUIRE) != 0u) {
        proc_resource_unpin(pin);
        return false;
    }
    g_render_pin = pin;
    return true;
}
void nv_render_end(void) {
    proc_t *pin = g_render_pin;
    g_render_pin = NULL;
    __atomic_store_n(&g_render_transaction, 0u, __ATOMIC_RELEASE);
    proc_resource_unpin(pin);
}

/* ----------------------------------------------------------------- classes */

#define FERMI_VASPACE_A             0x000090f1u
#define KEPLER_CHANNEL_GROUP_A      0x0000a06cu   /* the TSG                    */
#define FERMI_CONTEXT_SHARE_A       0x00009067u
#define NV_CTXSHARE_SUBCONTEXT_ASYNC 1u  /* NV_CTXSHARE_ALLOCATION_FLAGS_SUBCONTEXT_ASYNC (nvos.h:3232) */
#define BLACKWELL_CHANNEL_GPFIFO_B  0x0000ca6fu   /* GB20x channel (all engines)*/
#define BLACKWELL_USERMODE_A        0x0000c761u
#define BLACKWELL_DMA_COPY_B        0x0000cab5u   /* the copy engine   (2D)     */
#define BLACKWELL_B                 0x0000ce97u   /* the 3D/graphics class      */
#define BLACKWELL_COMPUTE_B         0x0000cec0u   /* the compute class          */
#define NVCFB0_VIDEO_DECODER        0x0000cfb0u   /* NVDEC  (decode)            */
#define NVCFB7_VIDEO_ENCODER        0x0000cfb7u   /* NVENC  (encode)            */
#define NV01_MEMORY_LOCAL_USER      0x00000040u   /* a chunk of the card's VRAM */

/* NVOS32 attributes for a contiguous VRAM allocation (nvos.h bitfields).
 * LOCATION is 26:25 (VIDMEM = 0); PHYSICALITY is 28:27 (CONTIGUOUS = 2). */
#define NVOS32_ATTR_VIDMEM_CONTIGUOUS   (2u << 27)          /* = 0x10000000 */
#define NVOS32_ATTR2_GPU_CACHEABLE_NO   0x00000002u         /* bits 3:2 = 2 -> <<2 */
#define NVOS32_ALLOC_FLAGS_NO_SCANOUT   0x00001000u
#define NVOS32_ALLOC_FLAGS_ALIGN_FORCE  0x00000100u
#define NVOS32_TYPE_IMAGE               0u
/* Scanout-surface params, taken verbatim from the real driver's
 * NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT path (nvkms-kapi.c:744-760): a display
 * surface is TYPE_PRIMARY (8), NOT TYPE_IMAGE, and must NOT carry NO_SCANOUT
 * (that marks it non-displayable and is why RM answered NV_ERR_NO_MEMORY for a
 * TYPE_IMAGE+NO_SCANOUT scanout request); it uses FORCE_MEM_GROWS_UP and the EVO
 * alignment 0x1000 (NV_EVO_SURFACE_ALIGNMENT, nvkms-types.h:88). */
#define NVOS32_TYPE_PRIMARY             8u
#define NVKMS_RM_HEAP_ID                0x0000DCBAu
#define NVOS32_ALLOC_FLAGS_FORCE_MEM_GROWS_UP 0x00000002u
#define NV_EVO_SURFACE_ALIGNMENT        0x1000u

/* NV0041 control: read a memory object's physical FB offset + aperture. */
#define NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR   0x00410103u
#define NV0041_APERTURE_VIDMEM                  0x00000000u

/* The subdevice handle nv_gsp_rm.c allocated, and the control that returns the
 * CE fault method-buffer size.  nouveau queries this rather than guessing, and
 * RM validates the channel's mthdbufMem against it. */
#define RM_SUBDEVICE                            0x5D1D0000u
#define NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE 0x20802a08u
#define NV2080_CTRL_CMD_FB_FLUSH_GPU_CACHE_IRQL 0x2080130du
#define NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE 0x20801112u

/* Enumerate the engines the card actually exposes, so we bind a channel to an
 * engine type that is really present (nouveau derives engineType from the HW
 * device-info table rather than assuming COPY0 - on some parts COPY0 is a
 * graphics-copy engine RM will not hand out as a standalone async copy). */
#define NV2080_CTRL_CMD_GPU_GET_ENGINES_V2      0x20800170u
#define NV2080_GPU_MAX_ENGINES_LIST_SIZE        0x54u
#define NV2080_ENGINE_TYPE_GRAPHICS_E           0x00000001u
#define NV2080_ENGINE_TYPE_COPY0_E              0x00000009u
#define NV2080_ENGINE_TYPE_COPY_LAST            0x00000012u   /* COPY9          */
#define NV2080_ENGINE_TYPE_NVDEC0_E             0x00000013u
#define NV2080_ENGINE_TYPE_NVDEC_LAST           0x0000001au
#define NV2080_ENGINE_TYPE_NVENC0_E             0x0000001bu
#define NV2080_ENGINE_TYPE_NVENC_LAST           0x0000001du
#define NV2080_ENGINE_TYPE_NVENC3_E             0x0000003fu

/* The exact per-class descriptor sizes RM checks (nouveau r535/r570 chan func:
 * gf100_chan_inst.size / gv100_chan_userd.size).  Backing allocations may be
 * larger, but the sizes we DESCRIBE to RM must be these. */
#define INST_DESC_SIZE      0x1000u    /* gf100_chan_inst.size  */
#define USERD_DESC_SIZE     0x200u     /* gv100_chan_userd.size */
#define RAMFC_DESC_SIZE     0x200u

/* The device handle nv_gsp_rm.c allocated the tree under (RM_DEVICE there). */
#define RM_DEVICE   0xDE1D0000u

/* Engine type numbers (verified vs OGKM cl2080_notification.h). */
#define NV2080_ENGINE_TYPE_GRAPHICS 0x00000001u
#define NV2080_ENGINE_TYPE_COPY0    0x00000009u
#define NV2080_ENGINE_TYPE_NVDEC0   0x00000013u   /* BSP   */
#define NV2080_ENGINE_TYPE_NVENC0   0x0000001bu   /* MSENC */

/* Control numbers (verified vs OGKM ctrl headers). */
#define NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY      0x00801813u
#define NV2080_CTRL_CMD_DMA_INVALIDATE_TLB           0x20802502u
#define NV0080_SET_PAGE_DIRECTORY_APERTURE_SYSMEM_COH 0x00000001u
#define NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES 0x90f10106u
#define SPLIT_VAS_SERVER_RM_MANAGED_VA_START         0x100000000ull
#define SPLIT_VAS_SERVER_RM_MANAGED_VA_SIZE          0x20000000ull
#define NVA06F_CTRL_CMD_BIND                        0xa06f0104u
#define NVA06F_CTRL_CMD_GPFIFO_SCHEDULE             0xa06f0103u
#define NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN 0xc36f0108u
#define NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST         0x0080170du

/* The channel's memory, laid out inside one sysmem pool.  Sizes are generous
 * and page-aligned; the GPU VA each lands at is fixed and arbitrary but high
 * enough to sit above RM's server-reserved split (4 GiB). */
#define POOL_BYTES          (2u * 1024u * 1024u)   /* 2 MiB: tables + rings    */
/* Match NVIDIA UVM's production default.  The earlier 256-entry assumption
 * came from a direct-GSP trace and does not describe a host-RM channel; the
 * live host-RM log instead failed when a long-lived producer revisited that
 * artificial 256-entry boundary.  Allocate and advertise all 1024 entries. */
#define GPFIFO_ENTRIES      1024u
#define GPFIFO_BYTES        (GPFIFO_ENTRIES * 8u)
#define USERD_BYTES         0x1000u                /* one page; GPPut at 0x8c  */
#define PUSHBUF_BYTES       0x80000u               /* 512 KiB: methods + stage */
/* Staging [0,64 KiB), methods [64 KiB,444 KiB), a dedicated HOST
 * retirement packet, then the existing 64-KiB present upload tail.
 * Neither a wrapped method stream nor a shader upload may alias the other. */
#define PB_METHOD_BEGIN     0x10000u
#define PB_TRACKER_OFF      0x6f000u
#define HOST_SEM_ADDR_LO    0x5cu
#define HOST_SEM_RELEASE_WFI (1u | (1u << 20))
#define SEM_BYTES           0x1000u
#define INSTANCE_BYTES      0x2000u                /* channel instance + RAMFC  */
#define MTHDBUF_BYTES       0x4000u                /* method buffer             */

#define VA_GPFIFO           0x00200000000ull       /* 8 GiB, above the split   */
#define VA_PUSHBUF          0x00200100000ull
#define VA_SEM              0x00200200000ull

/* USERD field offsets.  On Blackwell (clca6f = BlackwellAControlGPFifo) GP_GET
 * at 0x88 was RETIRED - only GP_PUT at 0x8c is live in USERD (0x00..0x8b is
 * Ignored00[0x23]).  The real, live GP_GET moved into RAMFC.  So we write GP_PUT
 * to USERD 0x8c but must read fetch progress (GP_GET) from RAMFC, not 0x88. */
#define USERD_GP_GET        0x88   /* DEAD on Blackwell - do not trust */
#define USERD_GP_PUT        0x8c

/* RAMFC field byte-offsets from the RAMFC base (NV_RAMFC_*, dev_ram.h gb100):
 * GP_BASE at word 36 (0x90), GP_BASE_HI word 37 (0x94), GP_GET word 38 (0x98).
 * RAMFC sits at the instance-block base (ramfcMem.base = inst_fb). */
#define RAMFC_GP_BASE       0x90
#define RAMFC_GP_BASE_HI    0x94
#define RAMFC_GP_GET        0x98

/* The usermode doorbell: BAR0 abs = FULL_PHYS_OFFSET(0xB80000) + 0x30090. */
#define NV_USERMODE_DOORBELL        (0x00b80000u + 0x00030090u)

/* CPU access to a VRAM (framebuffer) offset via the Hopper/Blackwell BAR0
 * window.  NV_XAL_EP_BAR0_WINDOW holds the 64 KiB-aligned base (fb >> 16); the
 * 1 MiB PRAMIN aperture at BAR0 + 0x700000 then shows that region.  This is how
 * we read/write USERD once it lives in VRAM (nouveau gh100_instmem does the
 * same for instance memory on GSP).  USERD is only a page, so the low 16 bits
 * index within the window. */
#define NV_XAL_EP_BAR0_WINDOW       0x0010fd40u
#define NV_BAR0_PRAMIN              0x00700000u
static void nv_fb_wr32(nv_card_t *c, u64 fb, u32 v) {
    nv_wr32(c, NV_XAL_EP_BAR0_WINDOW, (u32)(fb >> 16));
    nv_wr32(c, NV_BAR0_PRAMIN + (u32)(fb & 0xFFFFu), v);
}
static u32 nv_fb_rd32(nv_card_t *c, u64 fb) {
    nv_wr32(c, NV_XAL_EP_BAR0_WINDOW, (u32)(fb >> 16));
    return nv_rd32(c, NV_BAR0_PRAMIN + (u32)(fb & 0xFFFFu));
}

/* NVOS04 USERD-index flags that pin the channel to a chosen chid: RM derives
 * ChID from the USERD index we send (kernel_channel.c: USERD_INDEX_VALUE =
 * ChID % 8, PAGE_VALUE = ChID / 8).  chid 0 is RESERVED (rsvd_chids = 1 in the
 * r570 firmware), so the first usable channel is chid 1. */
#define NVOS04_USERD_INDEX_VALUE(chid)      (((u32)(chid) & 7u) << 8)   /* 10:8  */
#define NVOS04_USERD_INDEX_FIXED            (1u << 11)                  /* 11:11 */
#define NVOS04_USERD_INDEX_PAGE_VALUE(chid) (((u32)(chid) >> 3) << 12)  /* 20:12 */

/* ----------------------------------------------------------- param structs */

/* NV_VASPACE_ALLOCATION_PARAMETERS - NOT packed.  vaBase is 8-aligned after the
 * u32 bigPageSize, so the struct is 48 bytes; packed it would be 44, and RM
 * silently drops params whose size does not match the class (the same failure
 * the object-tree allocs hit). */
typedef struct { u32 index, flags; u64 va_size, va_start, va_limit; u32 big_page; u64 va_base; }
    vaspace_params_t;
_Static_assert(sizeof(vaspace_params_t) == 48,
               "NV_VASPACE_ALLOCATION_PARAMETERS is 48 bytes");

/* NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS (nvos.h:2900): NOT packed - the trailing
 * NvBool is padded so RM's compiler makes this 20 bytes, and RM rejects a short
 * (17-byte packed) struct with NV_ERR_INVALID_PARAMETER(59).  All fields are
 * 4-aligned so dropping `packed` keeps every offset and only fixes the size. */
typedef struct { u32 h_error, h_ecc_error, h_vaspace, engine_type; u8 vgpu_plugin; }
    tsg_params_t;
_Static_assert(sizeof(tsg_params_t) == 20, "NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS is 20 bytes");

typedef struct { u32 h_vaspace, flags, subctx_id; } __attribute__((packed)) ctxshare_params_t;

typedef struct { u64 phys_address; u32 num_entries, flags, h_vaspace, ch_id, subdevice_id, pasid; }
    __attribute__((packed)) set_page_dir_params_t;

/* NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS, ABI size 184. */
typedef struct {
    u32 h_subdevice;
    u32 subdevice_id;
    u64 page_size;
    u64 virt_addr_lo;
    u64 virt_addr_hi;
    u32 num_levels_to_copy;
    u32 _pad;
    nv_vmm_level_t levels[6];
} server_reserved_pdes_params_t;
_Static_assert(sizeof(server_reserved_pdes_params_t) == 184,
               "COPY_SERVER_RESERVED_PDES params are 184 bytes");

typedef struct { u32 version, engine_type; } __attribute__((packed)) copy_params_t;

/* NV_BSP/MSENC_ALLOCATION_PARAMETERS: {size, prohibitMultipleInstances,
 * engineInstance}.  Used for NVDEC/NVENC object allocation. */
typedef struct { u32 size, prohibit_multiple, engine_instance; }
    __attribute__((packed)) video_obj_params_t;

typedef struct { u32 engine_type; } __attribute__((packed)) bind_params_t;

/* 595.99.02 added bSkipEnable to the public schedule ABI.  Sending the old
 * two-byte form reaches the channel but is rejected by host RM with
 * NV_ERR_INVALID_ARGUMENT(31), exactly as the 00:36 hardware log showed. */
typedef struct { u8 enable, skip_submit, skip_enable; }
    __attribute__((packed)) schedule_params_t;
_Static_assert(sizeof(schedule_params_t) == 3,
               "595.99.02 NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS is 3 bytes");
typedef struct { u32 work_submit_token; } work_submit_token_params_t;
typedef struct {
    u32 num_channels;
    u32 _align_pointers;
    u64 p_channel_handle_list;
    u64 p_channel_list;
} channel_list_params_t;
_Static_assert(sizeof(channel_list_params_t) == 24,
               "NV0080_CTRL_FIFO_GET_CHANNELLIST_PARAMS is 24 bytes");

/* NV_MEMORY_DESC_PARAMS: base, size (u64, 8-aligned), addressSpace, cacheAttrib. */
typedef struct { u64 base, size; u32 address_space, cache_attrib; } __attribute__((packed)) mem_desc_t;

/* NV_CHANNEL_ALLOC_PARAMS - the layout from alloc_channel.h, with the reserved
 * CC tail kept so the size matches what RM reads.  NV_MAX_SUBDEVICES is 8. */
#define NV_MAX_SUBDEVICES 8
#define CC_IV_DWORDS 3
#define CC_NONCE_DWORDS 8
typedef struct {
    u32 h_object_error;
    u32 h_object_buffer;
    u64 gpfifo_offset;
    u32 gpfifo_entries;
    u32 flags;
    u32 h_context_share;
    u32 h_vaspace;
    u32 h_userd_memory[NV_MAX_SUBDEVICES];
    u64 userd_offset[NV_MAX_SUBDEVICES];
    u32 engine_type;
    u32 cid;
    u32 subdevice_id;
    u32 h_object_ecc_error;
    mem_desc_t instance_mem;
    mem_desc_t userd_mem;
    mem_desc_t ramfc_mem;
    mem_desc_t mthdbuf_mem;
    u32 h_phys_channel_group;
    u32 internal_flags;
    mem_desc_t error_notifier_mem;
    mem_desc_t ecc_error_notifier_mem;
    u32 process_id;
    u32 sub_process_id;
    u32 encrypt_iv[CC_IV_DWORDS];
    u32 decrypt_iv[CC_IV_DWORDS];
    u32 hmac_nonce[CC_NONCE_DWORDS];
    u32 tpc_config_id;   /* DTD-PG TPC config; added in r570 - MUST be present */
    /* RM's NV_CHANNEL_ALLOC_PARAMS is NOT packed: its u64 members give it 8-byte
     * alignment, so the compiler pads sizeof up from 364 to 368 after this final
     * u32.  nouveau sends that padded sizeof (368); the resource server checks the
     * RPC paramsSize against RS_REQUIRED(sizeof(NV_CHANNEL_ALLOC_PARAMS)) = 368 and
     * rejects a mismatch with INVALID_PARAMETER(59).  We keep the struct packed
     * (so every field offset is explicit and matches) and add the 4 trailing bytes
     * by hand, so we send exactly 368 like every real driver.  (r535 without
     * tpcConfigID ended at 360, already 8-aligned; r570's extra u32 forces 368.) */
    u32 _trailing_pad;
} __attribute__((packed)) channel_alloc_params_t;

/* Compile-time guards: these MUST match RM's structs byte-for-byte or GSP-RM
 * silently drops the alloc (the failure mode that cost days).  Verified vs
 * nouveau r570 nvrm/fifo.h - our firmware is 570.144, and r570 appended a
 * tpcConfigID u32 after hmacNonce, so NV_CHANNEL_ALLOC_PARAMS is 364 (r535 was
 * 360 - sending 360 to a 570 GSP-RM was rejected INVALID_PARAMETER 59).
 * NV_MEMORY_DESC_PARAMS = 24, ce.h NVC0B5 = 8, nvdec/nvenc.h = 12. */
_Static_assert(sizeof(mem_desc_t) == 24, "NV_MEMORY_DESC_PARAMS is 24 bytes");
_Static_assert(sizeof(channel_alloc_params_t) == 368,
               "NV_CHANNEL_ALLOC_PARAMS is 368 bytes (r570: 364 of fields + 4 trailing "
               "pad from the struct's natural 8-byte alignment - what real drivers send)");
_Static_assert((GPFIFO_ENTRIES & (GPFIFO_ENTRIES - 1u)) == 0,
               "the hardware GPFIFO ring size must be a power of two");
_Static_assert(GPFIFO_ENTRIES * 8u <= GPFIFO_BYTES,
               "the GPFIFO allocation must contain every advertised entry");
_Static_assert((GPFIFO_BYTES % PAGE_SIZE) == 0,
               "the allocation following the GPFIFO must remain page aligned");
_Static_assert(sizeof(copy_params_t) == 8, "NVC0B5_ALLOCATION_PARAMETERS is 8 bytes");
_Static_assert(sizeof(video_obj_params_t) == 12, "NV_BSP/MSENC params are 12 bytes");

/* The addressSpace values RM uses in NV_MEMORY_DESC_PARAMS. */
#define ADDR_SPACE_SYSMEM   1u
#define ADDR_SPACE_VIDMEM   2u

/* NVOS04 channel flags we set (bit positions from clc36f/fifo.h). */
#define NVOS04_FLAGS_CHANNEL_TYPE_PHYSICAL  0u   /* bits 1:0 = 0               */
#define NVOS04_FLAGS_USERD_PAGE_FIXED       (1u << 21)
/* PRIVILEGED_CHANNEL is bit 5 (nvrm/fifo.h NVOS04_FLAGS_PRIVILEGED_CHANNEL 5:5).
 * The GR GOLDEN channel MUST set it: RM runs the FECS/GPCCS golden-init microcode
 * against that channel, and a non-privileged golden channel yields a golden image
 * with wrong SM/SKED enable state - grids then schedule but no SM ever runs them
 * (r535_gr.c:305 passes priv=true; r535/fifo.c:98-101). Real work channels stay
 * non-privileged (nouveau uchan.c:387). */
#define NVOS04_FLAGS_PRIVILEGED_CHANNEL     (1u << 5)

/* internalFlags: PRIVILEGE in 1:0 (USER=0, ADMIN=1), ERROR_NOTIFIER_TYPE_NONE(1)
 * in 3:2, ECC_ERROR_NOTIFIER_TYPE_NONE(1) in 5:4 (r535/fifo.h). */
#define ERROR_NOTIFIER_TYPE_NONE            1u
#define INTERNALFLAGS_PRIVILEGE_ADMIN       1u
#define INTERNALFLAGS   ((0u) | (ERROR_NOTIFIER_TYPE_NONE << 2) | (ERROR_NOTIFIER_TYPE_NONE << 4))
/* Same, but PRIVILEGE=ADMIN(1) - for the golden channel (r535/fifo.c:143-146). */
#define INTERNALFLAGS_ADMIN ((INTERNALFLAGS_PRIVILEGE_ADMIN) | (ERROR_NOTIFIER_TYPE_NONE << 2) | (ERROR_NOTIFIER_TYPE_NONE << 4))

/* NV_MEMORY_ALLOCATION_PARAMS (nvos.h) - NOT packed: the u64 block is 8-aligned,
 * so a 4-byte hole sits after zcull_covg.  Field offsets match RM's compiler
 * (same alignment rules) -> 128 bytes.  We only fill owner/type/attr/attr2/size/
 * alignment/flags; the rest stay zero, exactly like a minimal vidmem alloc. */
typedef struct {
    u32 owner, type, flags;
    u32 width, height; s32 pitch;
    u32 attr, attr2, format, compr_covg, zcull_covg;
    u64 range_lo, range_hi, size, alignment, offset, limit, address;
    u32 ctag_offset, h_vaspace, internal_flags, tag; s32 numa_node;
} mem_alloc_params_t;
_Static_assert(sizeof(mem_alloc_params_t) == 128, "NV_MEMORY_ALLOCATION_PARAMS is 128 bytes");

/* NV0041_CTRL_GET_SURFACE_PHYS_ATTR_PARAMS - memOffset is the FB physical offset
 * we hand back to RM as the instance block's base. */
typedef struct {
    u64 mem_offset;
    u32 mem_format, compr_offset, compr_format, mem_aperture, gpu_cache_attr,
        gpu_p2p_cache_attr, mmu_context;
    u64 contig_segment_size;
} phys_attr_params_t;

/* ----------------------------------------------------------- the channels */
#include "nv_error_notifier.h"

typedef struct {
    bool      open;
    nv_card_t *card;
    nv_rm_t   *rm;

    void     *pool_va;         /* CPU pointer to the sysmem pool              */
    u64       pool_phys;       /* its physical address (= what the GPU walks) */

    nv_vmm_t  vmm;             /* OUR page tables over that pool              */

    u64       gpfifo_phys, userd_phys, pushbuf_phys, sem_phys;
    u64       userd_fb;        /* USERD's VRAM address at this chid's slot     */
    u64       userd_page_fb;   /* USERD VRAM page base (slot 0)                */
    u64       ramfc_fb;        /* RAMFC VRAM base; real GP_GET at +0x98        */
    volatile u32 *userd_bar1;  /* L2-coherent CPU view of USERD via BAR1, or 0 */
    volatile u32 *submit_bar1_flush; /* PCIe ordering read used by Linux UVM  */
    volatile u32 *userd;       /* CPU view of USERD's chid slot (sysmem path)  */
    bool      userd_sysmem;    /* USERD lives in the sysmem pool (coherent)    */
    u32       runlist_id;      /* real runlist for this engine (device-info)   */
    bool      runlist_known;   /* runlist_id was read from the device-info tbl */
    volatile u32 *gpfifo;      /* CPU view of the ring                        */
    volatile u8  *pushbuf;     /* CPU view of the pushbuffer                  */
    volatile u32 *sem;         /* CPU view of the completion semaphore        */

    u32       gp_put;          /* next free GPFIFO entry                      */
    u32       pb_at;           /* byte offset into the pushbuffer             */
    u32       completion_seq;  /* unique low-16 completion cookie             */
    bool      submit_failed;  /* never recycle storage after an unretired push */
    u32       method_wraps;
    u32       chid;            /* our channel id / doorbell vector            */
    u32       work_submit_token; /* authoritative host-RM doorbell payload     */
    bool      work_submit_token_valid;
    volatile nv_error_notification_t *error_notifier;

    u32       h_channel;       /* this channel's RM handle                    */
    u32       obj_class;       /* the primary engine object's class           */
    const char *name;
} nv_channel_t;

/* Index each engine's channel gets.  0 (copy) is the one selftest runs on. */
enum { CH_COPY = 0, CH_GFX, CH_COMPUTE, CH_NVDEC, CH_NVENC, CH_GOLDEN, CH_COUNT };
static nv_channel_t channels[CH_COUNT];

/* Never reuse a semaphore payload on a channel.  A late write from submission
 * N can otherwise arrive after the CPU clears the word for submission N+1 and
 * falsely certify the new job.  A unique cookie makes the semaphore itself the
 * authoritative completion proof even on GB202 channels whose RAMFC GP_GET
 * shadow is stale (the 06:22 COPY4 boot wrote 0x5a5a while GP_GET remained 0). */
static u32 next_completion_signal(nv_channel_t *ch, u32 tag) {
    u32 seq = (++ch->completion_seq) & 0xffffu;
    if (!seq) seq = (++ch->completion_seq) & 0xffffu;
    return (tag & 0xffff0000u) | seq;
}

/* Per-channel RM handles: unique by index (the low nibble). */
static u32 h_vaspace(int idx)  { return 0x90f10000u + (u32)idx; }
static u32 h_tsg(int idx)      { return 0xa06c0000u + (u32)idx; }
static u32 h_ctxshare(int idx) { return 0x90670000u + (u32)idx; }
static u32 h_group(int idx)    { return 0xa06c0000u + (u32)idx; }   /* explicit TSG per GR channel */
static u32 h_channel(int idx)  { return 0xca6f0000u + (u32)idx; }
static u32 h_object(int idx)   { return 0xcab50000u + (u32)idx; }
static u32 h_object2(int idx)  { return 0xcec00000u + (u32)idx; }
static u32 h_instvram(int idx) { return 0x00400000u + (u32)idx; }
static u32 h_userdvram(int idx) { return 0x00410000u + (u32)idx; }
static u32 h_tablesvram(int idx) { return 0x00420000u + (u32)idx; }

static int nv_channel_index(const nv_channel_t *ch) {
    if (ch < &channels[0] || ch >= &channels[CH_COUNT]) return -1;
    return (int)(ch - &channels[0]);
}

static u64 pool_phys_at(nv_channel_t *ch, u32 off) { return ch->pool_phys + off; }

extern u32 nvrm_host_map_memory(u32 client, u32 device, u32 memory,
                                u64 offset, u64 length, void **address,
                                u32 flags);
extern u32 nvrm_transfer_rm_memory(u32 client, u32 memory, u64 offset,
                                   void *buffer, u64 size, u8 read);
extern u32 nvrm_host_mark_context_bound(u32 client, u32 channel);
extern u32 nvrm_host_get_video_falcon_context(
    u32 client, u32 channel,
    u64 *alignment, u64 *size, u64 *phys_addr,
    u32 *aperture, u32 *kind, u32 *page_size, u8 *contiguous, u8 *privileged);

#include "nv_codec_notifier_setup.h"

/* The engines the card reports as present (NV2080_ENGINE_TYPE_*), queried once
 * from GET_ENGINES_V2 and cached.  count == 0 means "not queried / query
 * failed" and callers fall back to their requested type. */
static u32 g_engine_list[NV2080_GPU_MAX_ENGINES_LIST_SIZE];
static u32 g_engine_count;

static void nv_query_engines(nv_card_t *c, nv_rm_t *rm) {
    struct { u32 count; u32 list[NV2080_GPU_MAX_ENGINES_LIST_SIZE]; } p;
    memset(&p, 0, sizeof p);
    u32 got = 0;
    if (!nv_rm_control(c, rm, RM_SUBDEVICE, NV2080_CTRL_CMD_GPU_GET_ENGINES_V2,
                       &p, sizeof p, &p, sizeof p, &got) ||
        got < 4 || p.count == 0 || p.count > NV2080_GPU_MAX_ENGINES_LIST_SIZE) {
        kwarn("nv-chan", "could not read the card's engine list; using requested engine types");
        g_engine_count = 0;
        return;
    }
    g_engine_count = p.count;
    for (u32 i = 0; i < p.count; i++) g_engine_list[i] = p.list[i];
    /* Log the whole list once - the first time we have ever seen exactly which
     * engines this card exposes. */
    char buf[256]; int n = 0;
    for (u32 i = 0; i < p.count && n < (int)sizeof buf - 8; i++)
        n += snprintf(buf + n, sizeof buf - n, "%u ", p.list[i]);
    kinfo("nv-chan", "the card reports %u engine(s): %s", p.count, buf);
}

static bool nv_engine_present(u32 type) {
    if (g_engine_count == 0) return true;   /* unknown -> trust the caller */
    for (u32 i = 0; i < g_engine_count; i++) if (g_engine_list[i] == type) return true;
    return false;
}

/* Return an engine of the same family that the card actually has, preferring the
 * requested one. NVENC3 is outside the NVENC0..2 contiguous band. */
static u32 nv_pick_engine(u32 requested) {
    if (nv_engine_present(requested) || g_engine_count == 0) return requested;
    if ((requested >= NV2080_ENGINE_TYPE_NVENC0_E &&
         requested <= NV2080_ENGINE_TYPE_NVENC_LAST) ||
        requested == NV2080_ENGINE_TYPE_NVENC3_E) {
        const u32 encoders[] = { NV2080_ENGINE_TYPE_NVENC0_E,
            NV2080_ENGINE_TYPE_NVENC0_E + 1u, NV2080_ENGINE_TYPE_NVENC_LAST,
            NV2080_ENGINE_TYPE_NVENC3_E };
        for (u32 i = 0; i < 4; i++) if (nv_engine_present(encoders[i])) {
            kinfo("nv-chan", "engine %u not present; using %u instead", requested, encoders[i]);
            return encoders[i];
        }
        return requested; /* No encoder found: let RM reject the allocation. */
    }
    u32 lo = requested, hi = requested;
    if (requested >= NV2080_ENGINE_TYPE_COPY0_E && requested <= NV2080_ENGINE_TYPE_COPY_LAST) {
        lo = NV2080_ENGINE_TYPE_COPY0_E; hi = NV2080_ENGINE_TYPE_COPY_LAST;
    } else if (requested >= NV2080_ENGINE_TYPE_NVDEC0_E && requested <= NV2080_ENGINE_TYPE_NVDEC_LAST) {
        lo = NV2080_ENGINE_TYPE_NVDEC0_E; hi = NV2080_ENGINE_TYPE_NVDEC_LAST;
    }
    for (u32 t = lo; t <= hi; t++) if (nv_engine_present(t)) {
        kinfo("nv-chan", "engine %u not present; using %u instead", requested, t);
        return t;
    }
    return requested;   /* nothing of the family found - let RM speak */
}

/* Pick a copy engine that is an INDEPENDENT async CE, NOT a GRCE.  This is the
 * fix for the copy engine never fetching (GP_GET stuck at 0 with no fault): on
 * GB202 the low LCEs COPY0..COPY3 are GRCEs (NV_CE_GRCE_MASK @ BAR0 0x1040d8,
 * bits 9:0, default 0x00f) bound to GRAPHICS' runlist.  A "lone GRCE" - a copy
 * channel on GR's runlist with no GR channel/context alive there - is one that
 * GSP-RM's host scheduler will NOT run (nouveau's r535_fifo_runl_ctor deletes
 * lone GRCEs outright).  BIND and GPFIFO_SCHEDULE both ACK because they only
 * flip this channel's own enable bit, but the runlist itself is never made live,
 * so the host never fetches - exactly the wall we hit with COPY0.  COPY4/COPY5
 * are async CEs on their own runlist and schedule standalone.  Read the live
 * GRCE mask to confirm, then pick the lowest present COPY engine whose GRCE bit
 * is clear.  (This card reports engines 13/14 = COPY4/COPY5 present.) */
static u32 nv_pick_async_copy_engine(nv_card_t *c) {
    u32 grce = nv_rd32(c, 0x001040d8u);
    bool poison = (grce & 0xFFFF0000u) == 0xBADF0000u;   /* PLM-blocked read */
    u32 mask = poison ? 0x00Fu : (grce & 0x3FFu);        /* default: COPY0..3 = GRCE */
    kinfo("nv-chan", "NV_CE_GRCE_MASK(0x1040d8)=%#x -> GRCE bits %#x%s",
          grce, mask, poison ? " (poison; assuming default 0x00f)" : "");
    for (u32 t = NV2080_ENGINE_TYPE_COPY0_E; t <= NV2080_ENGINE_TYPE_COPY_LAST; t++) {
        u32 bit = t - NV2080_ENGINE_TYPE_COPY0_E;
        if (nv_engine_present(t) && !(mask & (1u << bit))) {
            kinfo("nv-chan", "using async copy engine type %u (COPY%u) - not a GRCE, "
                             "schedules on its own runlist", t, bit);
            return t;
        }
    }
    kwarn("nv-chan", "no independent async CE present; falling back to COPY0 (GRCE) - "
                     "the host may refuse to schedule it");
    return NV2080_ENGINE_TYPE_COPY0_E;
}

/* NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE layout.  engineData index 2 =
 * RM_ENGINE_TYPE (numerically == our NV2080 type for GR and COPY: COPY0=9), and
 * index 3 = RUNLIST - the real per-engine runlist id.  This is the ONLY
 * authoritative source of a channel's runlist; the doorbell must carry it
 * (BIT30 | (runlist<<16) | chid).  GB202 has ~30 Esched engines each on its own
 * runlist, so COPY4's id is typically >= 8 and a 0..7 sweep can never reach it -
 * exactly the wall behind GP_GET=0.  (Matches nouveau r535_fifo_runl_ctor.) */
#define ENGINE_INFO_TYPE_RM_ENGINE_TYPE 2u
#define ENGINE_INFO_TYPE_RUNLIST        3u
#define DEV_INFO_MAX_ENTRIES            32u
#define DEV_INFO_ENGINE_DATA_TYPES      16u
typedef struct {
    u32  engine_data[DEV_INFO_ENGINE_DATA_TYPES];
    u32  pbdma_ids[2];
    u32  pbdma_fault_ids[2];
    u32  num_pbdmas;
    char engine_name[16];
} __attribute__((packed)) dev_info_entry_t;
typedef struct {
    u32 base_index;
    u32 num_entries;
    u8  more;
    u8  _pad[3];               /* NvBool@8 then align entries[] (u32-first) to 12 */
    dev_info_entry_t entries[DEV_INFO_MAX_ENTRIES];
} __attribute__((packed)) dev_info_table_t;
_Static_assert(sizeof(dev_info_entry_t) == 100, "device-info entry is 100 bytes");
_Static_assert(sizeof(dev_info_table_t) == 12u + 32u * 100u, "device-info table is 3212 bytes");

/* Look up the real runlist id for engine_type (an NV2080/RM engine type, equal
 * for GR/COPY).  Logs the whole table (name + rmtype + runlist) so one boot maps
 * every engine to its runlist.  Returns true and *out_runlist on a match. */
/* The device-info table reports engines by RM_ENGINE_TYPE (engineData[2]), which
 * is NOT the NV2080_ENGINE_TYPE we bind channels with.  They coincide for GR and
 * COPY (GR0=1, COPY0=9 in both enums) - which is why COPY4 already worked - but
 * DIVERGE for video: NV2080 NVDEC0=0x13 -> RM 0x1d (+10), NV2080 NVENC0=0x1b ->
 * RM 0x25.  Convert before matching or the NVDEC/NVENC runlist lookup fails.
 * (Values verified vs ogkm gpu_engine_type.h: RM GR0=1/COPY0=9/NVDEC0=0x1d/
 * NVENC0=0x25, and class/cl2080_notification.h: NV2080 GRAPHICS=1/COPY0=9/
 * NVDEC0(BSP)=0x13/NVENC0(MSENC)=0x1b.) */
static u32 nv2080_to_rm_engine_type(u32 nv2080) {
    if (nv2080 >= NV2080_ENGINE_TYPE_GRAPHICS && nv2080 <= 8)
        return nv2080;                                   /* GR0..7  == RM 1..8   */
    if (nv2080 >= NV2080_ENGINE_TYPE_COPY0 && nv2080 <= NV2080_ENGINE_TYPE_COPY0 + 9)
        return nv2080;                                   /* COPY0..9 == RM 9..18 */
    if (nv2080 >= NV2080_ENGINE_TYPE_NVDEC0 && nv2080 <= NV2080_ENGINE_TYPE_NVDEC0 + 7)
        return 0x1du + (nv2080 - NV2080_ENGINE_TYPE_NVDEC0);   /* NVDEC0..7 */
    if (nv2080 >= NV2080_ENGINE_TYPE_NVENC0 && nv2080 <= NV2080_ENGINE_TYPE_NVENC0 + 2)
        return 0x25u + (nv2080 - NV2080_ENGINE_TYPE_NVENC0);   /* NVENC0..2 */
    /* cl2080_notification.h: NVENC3=0x3f; gpu_engine_type.h: RM NVENC3=0x28. */
    if (nv2080 == NV2080_ENGINE_TYPE_NVENC3_E) return 0x28u;
    return nv2080;                                        /* unknown: pass through */
}

static bool nv_query_runlist(nv_card_t *c, nv_rm_t *rm, u32 engine_type,
                             u32 *out_runlist) {
    u32 want_rm = nv2080_to_rm_engine_type(engine_type);
    static dev_info_table_t t;   /* 3.2 KiB - static, never on the stack */
    memset(&t, 0, sizeof t);
    u32 got = 0;
    if (!nv_rm_control(c, rm, RM_SUBDEVICE, NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE,
                       &t, sizeof t, &t, sizeof t, &got) || got < 8) {
        kwarn("nv-chan", "device-info table query failed (got %u); real runlist unknown", got);
        return false;
    }
    static bool dumped = false;   /* the table is identical every call; log it once */
    u32 n = t.num_entries;
    if (n > DEV_INFO_MAX_ENTRIES) n = DEV_INFO_MAX_ENTRIES;
    bool found = false;
    for (u32 i = 0; i < n; i++) {
        u32 rmtype = t.entries[i].engine_data[ENGINE_INFO_TYPE_RM_ENGINE_TYPE];
        u32 runl   = t.entries[i].engine_data[ENGINE_INFO_TYPE_RUNLIST];
        t.entries[i].engine_name[15] = '\0';        /* make %s safe */
        if (!dumped)
            kinfo("nv-chan", "  devinfo[%u]: rmType %u runlist %u '%s'",
                  i, rmtype, runl, t.entries[i].engine_name);
        if (rmtype == want_rm) { *out_runlist = runl; found = true; }
    }
    dumped = true;
    if (found)
        kinfo("nv-chan", "engine %u (RM %u) -> REAL runlist id %u (device-info table)",
              engine_type, want_rm, *out_runlist);
    else
        kwarn("nv-chan", "engine %u (RM %u) not found in device-info table; runlist unknown",
              engine_type, want_rm);
    return found;
}

/* Allocate a contiguous chunk of the card's VRAM through RM and return its
 * physical FB offset.  On a GSP-client card the channel's instance block MUST
 * live in FB (kchannelAllocMem_GM107 forces ADDRLIST_FBMEM_ONLY for
 * IS_GSP_CLIENT), so we cannot back it with sysmem - we allocate an
 * NV01_MEMORY_LOCAL_USER object and read its offset with GET_SURFACE_PHYS_ATTR.
 * Returns true and writes *fb_offset on success. */
bool nv_vram_alloc(nv_card_t *c, nv_rm_t *rm, u32 handle, u64 size,
                          u64 *fb_offset) {
    mem_alloc_params_t mp = { 0 };
    mp.owner     = 0x4b455354u;               /* 'KEST' - a debug tag          */
    mp.type      = NVOS32_TYPE_IMAGE;
    mp.attr      = NVOS32_ATTR_VIDMEM_CONTIGUOUS;
    mp.attr2     = (NVOS32_ATTR2_GPU_CACHEABLE_NO << 2);
    mp.flags     = NVOS32_ALLOC_FLAGS_NO_SCANOUT | NVOS32_ALLOC_FLAGS_ALIGN_FORCE;
    mp.size      = size;
    mp.alignment = 0x10000;                   /* 64 KiB, the big-page size     */
    if (!nv_rm_alloc(c, rm, RM_DEVICE, handle, NV01_MEMORY_LOCAL_USER,
                     &mp, sizeof mp)) {
        kerr("nv-chan", "could not allocate %llu bytes of VRAM for the instance block",
             (unsigned long long)size);
        return false;
    }

    phys_attr_params_t pa = { 0 };
    u32 got = 0;
    if (!nv_rm_control(c, rm, handle, NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR,
                       &pa, sizeof pa, &pa, sizeof pa, &got) ||
        got < sizeof pa) {
        kerr("nv-chan", "VRAM allocated but its physical address could not be read");
        return false;
    }
    if (pa.mem_aperture != NV0041_APERTURE_VIDMEM) {
        kerr("nv-chan", "the instance block did not land in VRAM (aperture %u)",
             pa.mem_aperture);
        return false;
    }
    *fb_offset = pa.mem_offset;
    kinfo("nv-chan", "instance block: %llu bytes of VRAM at FB offset %#llx",
          (unsigned long long)size, (unsigned long long)pa.mem_offset);
    return true;
}

/* Allocate a DISPLAY SCANOUT surface in VRAM, using the exact NVOS32 params the
 * real driver uses for a scanned-out surface (nvkms-kapi.c:744-760): TYPE_PRIMARY
 * (not IMAGE), contiguous VIDMEM, pitch layout, GPU-uncached, ALIGNMENT_FORCE +
 * FORCE_MEM_GROWS_UP, EVO alignment - and crucially WITHOUT NO_SCANOUT.  Our
 * generic nv_vram_alloc uses TYPE_IMAGE|NO_SCANOUT which RM refused with
 * NV_ERR_NO_MEMORY (0x51) for a 22MB display surface.  Same phys-attr readback. */
bool nv_vram_alloc_scanout(nv_card_t *c, nv_rm_t *rm, u32 handle, u64 size,
                           u64 *fb_offset) {
    mem_alloc_params_t mp = { 0 };
    /* Match NVKMS exactly here.  This is not just a debug label: display
     * allocations in the 595 RM consistently use NVKMS_RM_HEAP_ID. */
    mp.owner     = NVKMS_RM_HEAP_ID;
    mp.type      = NVOS32_TYPE_PRIMARY;       /* scanout surface (kapi:751)    */
    mp.attr      = NVOS32_ATTR_VIDMEM_CONTIGUOUS; /* VIDMEM|CONTIGUOUS|PITCH   */
    mp.attr2     = (NVOS32_ATTR2_GPU_CACHEABLE_NO << 2);
    mp.flags     = NVOS32_ALLOC_FLAGS_ALIGN_FORCE | NVOS32_ALLOC_FLAGS_FORCE_MEM_GROWS_UP;
    mp.size      = size;
    mp.alignment = NV_EVO_SURFACE_ALIGNMENT;  /* 0x1000 (kapi:752/types.h:88)  */
    if (!nv_rm_alloc(c, rm, RM_DEVICE, handle, NV01_MEMORY_LOCAL_USER,
                     &mp, sizeof mp)) {
        u32 primary_status = nv_last_alloc_status;
        /* TYPE_PRIMARY is classified as an ISO allocation by standard_mem.c.
         * The 5070 Ti's GSP heap can have plenty of ordinary VRAM yet no large
         * enough contiguous ISO region (observed status 0x51 while a 52 MiB
         * IMAGE allocation succeeded).  A pitch surface addressed through
         * CA7E TARGET_PHYSICAL_NVM only needs display-readable contiguous local
         * memory.  Retry as TYPE_IMAGE without NO_SCANOUT: this uses the normal
         * local heap but deliberately remains eligible for scanout. */
        memset(&mp, 0, sizeof mp);
        mp.owner     = NVKMS_RM_HEAP_ID;
        mp.type      = NVOS32_TYPE_IMAGE;
        mp.attr      = NVOS32_ATTR_VIDMEM_CONTIGUOUS;
        mp.attr2     = (NVOS32_ATTR2_GPU_CACHEABLE_NO << 2);
        mp.flags     = NVOS32_ALLOC_FLAGS_ALIGN_FORCE |
                       NVOS32_ALLOC_FLAGS_FORCE_MEM_GROWS_UP;
        mp.size      = size;
        mp.alignment = NV_EVO_SURFACE_ALIGNMENT;
        kwarn("nv-chan", "TYPE_PRIMARY scanout allocation returned %#x; retrying "
                         "as contiguous scanout-allowed TYPE_IMAGE", primary_status);
        if (!nv_rm_alloc(c, rm, RM_DEVICE, handle, NV01_MEMORY_LOCAL_USER,
                         &mp, sizeof mp)) {
            kerr("nv-chan", "could not allocate %llu bytes of display-readable VRAM "
                            "(PRIMARY %#x, IMAGE %#x)",
                 (unsigned long long)size, primary_status, nv_last_alloc_status);
            return false;
        }
    }
    phys_attr_params_t pa = { 0 };
    u32 got = 0;
    if (!nv_rm_control(c, rm, handle, NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR,
                       &pa, sizeof pa, &pa, sizeof pa, &got) || got < sizeof pa) {
        kerr("nv-chan", "scanout VRAM allocated but its physical address could not be read");
        return false;
    }
    if (pa.mem_aperture != NV0041_APERTURE_VIDMEM) {
        kerr("nv-chan", "the scanout surface did not land in VRAM (aperture %u)", pa.mem_aperture);
        return false;
    }
    *fb_offset = pa.mem_offset;
    kinfo("nv-chan", "scanout surface: %llu bytes of VRAM at FB offset %#llx (%s, scanout allowed)",
          (unsigned long long)size, (unsigned long long)pa.mem_offset,
          mp.type == NVOS32_TYPE_PRIMARY ? "TYPE_PRIMARY" : "TYPE_IMAGE fallback");
    return true;
}

/* ============================ GR golden context ==========================
 * 3D (BLACKWELL_B 0xce97) and compute (BLACKWELL_COMPUTE_B 0xcec0) run on the
 * GRAPHICS engine and cannot draw/dispatch until a GR "golden context" has been
 * captured by RM and each channel has had that context PROMOTED into it.  The
 * copy engine needs none of this; that is why 2D already works and 3D/compute do
 * not.  Everything below is a faithful port of nouveau's r535/r570 gr.c path,
 * with every constant cross-checked against the reference headers.
 * ---- verified vs local upstream references\linux-nouveau ...\r535\gr.c,
 *      r535/nvrm/gr.h, r570/nvrm/gr.h, and ogkm ctrl2080gpu.h. */

#define NV2080_CTRL_CMD_GPU_PROMOTE_CTX                    0x2080012bu
#define NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_CTXBUFS_INFO   0x20800a32u
#define NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO             0x20801219u
#define NV2080_CTRL_CMD_KGR_GET_CTX_BUFFER_PTES            0x20800a28u

/* NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY - 32 bytes (ctrl2080gpu.h:893). */
typedef struct {
    u64 gpu_phys_addr;   /*  0 */
    u64 gpu_virt_addr;   /*  8 */
    u64 size;             /* 16 */
    u32 phys_attr;        /* 24 */
    u16 buffer_id;         /* 28 */
    u8  b_initialize;      /* 30 */
    u8  b_nonmapped;       /* 31 */
} __attribute__((packed)) promote_entry_t;
_Static_assert(sizeof(promote_entry_t) == 32, "PROMOTE_CTX entry is 32 bytes");

/* NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS - 560 bytes (ctrl2080gpu.h:959). */
typedef struct {
    u32 engine_type;      /*  0 - always 1 (GRAPHICS), even for compute channels */
    u32 h_client;          /*  4 */
    u32 ch_id;              /*  8 - deprecated, 0 */
    u32 h_chan_client;     /* 12 */
    u32 h_object;          /* 16 - the channel handle */
    u32 h_virt_memory;     /* 20 - 0 (entries carry addresses) */
    u64 virt_address;      /* 24 - 0 */
    u64 size;                /* 32 - 0 */
    u32 entry_count;       /* 40 */
    u32 _pad;                /* 44 - align promoteEntry[] to 8 */
    promote_entry_t entry[16];   /* 48..560 */
} __attribute__((packed)) promote_ctx_params_t;
_Static_assert(sizeof(promote_ctx_params_t) == 560, "PROMOTE_CTX params is 560 bytes");

/* Host-RM external-VAS resource binding.  These are exact narrow views of
 * ctrl2080gr.h / ctrl2080internal.h / ctrl2080flcn.h from the linked 595.99.02
 * driver.  Object construction allocates and physically initializes these
 * buffers; a client-owned page table must then map every resource and provide
 * its VA through one virtual PROMOTE_CTX, as nvGpuOpsBindChannelResources does. */
#define HOST_GR_CTX_MAX 64u
#define HOST_GR_PTES_MAX 128u
typedef struct {
    u64 alignment, size, buffer_handle, page_count, phys_addr;
    u32 buffer_type, aperture, kind, page_size;
    u8  is_contiguous, is_global, is_local, device_descendant;
    u8  uuid[16];
    u8  _tail[4];
} host_gr_ctx_info_t;
_Static_assert(sizeof(host_gr_ctx_info_t) == 80,
               "NV2080_CTRL_GR_CTX_BUFFER_INFO is 80 bytes");
typedef struct {
    u32 h_user_client, h_channel, buffer_count, _align_entries;
    host_gr_ctx_info_t info[HOST_GR_CTX_MAX];
} host_gr_ctx_info_params_t;
_Static_assert(sizeof(host_gr_ctx_info_params_t) == 5136,
               "NV2080_CTRL_GR_GET_CTX_BUFFER_INFO_PARAMS is 5136 bytes");
typedef struct {
    u32 h_user_client, h_channel, buffer_type, first_page, num_pages, _align_addrs;
    u64 phys_addrs[HOST_GR_PTES_MAX];
    u8  no_more_pages;
    u8  _tail[7];
} host_gr_ptes_params_t;
_Static_assert(sizeof(host_gr_ptes_params_t) == 1056,
               "NV2080_CTRL_KGR_GET_CTX_BUFFER_PTES_PARAMS is 1056 bytes");
typedef struct {
    u32 h_user_client, h_channel;
    u64 alignment, size, buffer_handle, page_count, phys_addr;
    u32 aperture, kind, page_size;
    u8  is_contiguous, device_descendant;
    u8  uuid[16];
    u8  _tail[2];
} host_flcn_ctx_info_params_t;
_Static_assert(sizeof(host_flcn_ctx_info_params_t) == 80,
               "NV2080_CTRL_FLCN_GET_CTX_BUFFER_INFO_PARAMS is 80 bytes");

/* The context-buffers-info reply (r570 nvrm/gr.h): 8 engines x 0x1a slots of
 * {size, alignment}.  We run 570.144 firmware so use 0x1a (r535 was 0x19). */
#define GR_ENGINE_ID_COUNT 0x1au
#define GR_MAX_ENGINES     8u
typedef struct { u32 size, alignment; } gr_ctxbuf_info_t;
typedef struct { gr_ctxbuf_info_t engine[GR_MAX_ENGINES][GR_ENGINE_ID_COUNT]; }
    __attribute__((packed)) gr_ctxbufs_params_t;
_Static_assert(sizeof(gr_ctxbufs_params_t) == 8u * 0x1au * 8u, "ctxbufs params 1664 bytes");

/* NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_* (r535 nvrm/gr.h). */
#define GR_BUF_MAIN              0u
#define GR_BUF_PM                1u
#define GR_BUF_PATCH             2u
#define GR_BUF_BUFFER_BUNDLE_CB  3u
#define GR_BUF_PAGEPOOL          4u
#define GR_BUF_ATTRIBUTE_CB      5u
#define GR_BUF_RTV_CB_GLOBAL     6u
#define GR_BUF_FECS_EVENT        9u
#define GR_BUF_PRIV_ACCESS_MAP   10u
#define GR_BUF_UNRESTRICTED_PRIV_ACCESS_MAP 11u

/* NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_* (id0), the index
 * into the reply's engine[] array (r535 nvrm/gr.h). */
#define GR_EID_GRAPHICS         0x00u
#define GR_EID_PAGEPOOL         0x0du
#define GR_EID_PATCH            0x10u
#define GR_EID_BUNDLE_CB        0x11u
#define GR_EID_ATTRIBUTE_CB     0x13u
#define GR_EID_RTV_CB_GLOBAL    0x14u
#define GR_EID_FECS_EVENT       0x17u
#define GR_EID_PRIV_ACCESS_MAP  0x18u

/* The id0 -> (bufferId, global, init, ro) map, exactly nouveau's map[] table
 * (r535 gr.c:185-198). */
static const struct { u8 id0; u8 buffer_id; u8 global, init, ro; } gr_ctxbuf_map[] = {
    { GR_EID_GRAPHICS,        GR_BUF_MAIN,             0, 1, 0 },
    { GR_EID_PATCH,           GR_BUF_PATCH,            0, 1, 0 },
    { GR_EID_BUNDLE_CB,       GR_BUF_BUFFER_BUNDLE_CB, 1, 0, 0 },
    { GR_EID_PAGEPOOL,        GR_BUF_PAGEPOOL,         1, 0, 0 },
    { GR_EID_ATTRIBUTE_CB,    GR_BUF_ATTRIBUTE_CB,     1, 0, 0 },
    { GR_EID_RTV_CB_GLOBAL,   GR_BUF_RTV_CB_GLOBAL,    1, 0, 0 },
    { GR_EID_FECS_EVENT,      GR_BUF_FECS_EVENT,       1, 1, 0 },
    { GR_EID_PRIV_ACCESS_MAP, GR_BUF_PRIV_ACCESS_MAP,  1, 1, 1 },
};

typedef struct {
    u16 buffer_id; u32 size; u8 page; u8 global, init, ro; u64 golden_fb;
} gr_ctxbuf_desc_t;
static gr_ctxbuf_desc_t g_ctxbuf[16];
static int  g_ctxbuf_nr;
static bool g_golden_ready;

static u32 order_base_2_u32(u32 v) { u32 n = 0; if (v > 1) { v--; while (v) { v >>= 1; n++; } } return n; }

/* Resolve the RM Memory handle backing a channel's VMM.  All channel VMMs are
 * embedded in the fixed channels[] array, so this remains exact and cannot
 * accidentally upload into another allocation. */
static u32 nv_vmm_memory_handle(nv_vmm_t *vmm) {
    for (int i = 0; i < CH_COUNT; i++)
        if (&channels[i].vmm == vmm) return h_tablesvram(i);
    return 0;
}

/* Copy the freshly-built page tables from the sysmem shadow into their VRAM
 * home.  Host RM must use MemUtils/CE because its FB_FLUSH control is not
 * exported and PRAMIN bypasses L2.  The direct-GSP backend retains its BAR0
 * upload followed by the supported cache flush. */
static bool nv_vmm_push_to_vram(nv_card_t *c, nv_rm_t *rm, nv_vmm_t *vmm) {
    u64 bytes = (u64)vmm->used * PAGE_SIZE;
    if (rm->host_api) {
        u32 memory = nv_vmm_memory_handle(vmm);
        if (!memory) {
            kerr("nv-chan", "host-RM VMM is not owned by a channel");
            return false;
        }
        u32 status = nvrm_transfer_rm_memory(rm->client, memory, 0,
                                              vmm->pool, bytes, false);
        if (status != 0) {
            kerr("nv-chan", "coherent host-RM page-table upload failed (%u)", status);
            return false;
        }
        return true;
    }
    for (u32 o = 0; o < bytes; o += 4)
        nv_fb_wr32(c, vmm->pool_gpu + o, *(const u32 *)(vmm->pool + o));
    u32 f = 0x3u;   /* WRITE_BACK_YES | INVALIDATE_YES */
    if (!nv_rm_control(c, rm, RM_SUBDEVICE,
                       NV2080_CTRL_CMD_FB_FLUSH_GPU_CACHE_IRQL,
                       &f, sizeof f, NULL, 0, NULL))
        return false;
    return true;
}

/* Page-table upload and translation-cache visibility are separate operations.
 * NVIDIA's nvGpuOpsInvalidateTlb issues this exact subdevice control after a
 * client updates an externally-owned VAS.  Without it, head0 can populate and
 * render, then head1's newly-added PTE remains cached as absent and faults the
 * copy channel before its semaphore write. */
static bool nv_vmm_invalidate(nv_card_t *c, nv_rm_t *rm, u32 h_vas,
                              const char *owner) {
    if (rm->host_api) {
        extern u32 nvrm_invalidate_external_root(u32 client, u32 memory, u64 root);
        int idx;
        for (idx = 0; idx < CH_COUNT; idx++)
            if (h_vaspace(idx) == h_vas) break;
        if (idx == CH_COUNT) return false;
        nv_vmm_t *vmm = &channels[idx].vmm;
        u32 status = nvrm_invalidate_external_root(rm->client, h_tablesvram(idx),
                                                   vmm->root_gpu);
        if (status) {
            kerr("nv-chan", "%s: external-root TLB invalidate failed (%u)", owner, status);
            return false;
        }
        return true;
    }
    struct {
        u32 h_client;      /* deprecated, kept for the 595 ABI */
        u32 h_device;      /* deprecated, kept for the 595 ABI */
        u32 engine;        /* deprecated */
        u32 h_vaspace;
    } p = { .h_vaspace = h_vas };
    bool ok = nv_rm_control(c, rm, RM_SUBDEVICE,
                            NV2080_CTRL_CMD_DMA_INVALIDATE_TLB,
                            &p, sizeof p, NULL, 0, NULL);
    if (!ok)
        kerr("nv-chan", "%s: RM refused the required VAS TLB invalidate (%u)",
             owner, nv_last_control_status);
    return ok;
}

static bool nv_vmm_commit(nv_card_t *c, nv_rm_t *rm, nv_vmm_t *vmm,
                          u32 h_vas, const char *owner) {
    return nv_vmm_push_to_vram(c, rm, vmm) &&
           nv_vmm_invalidate(c, rm, h_vas, owner);
}

/* Native CPU-RM allocates and physically initializes GR/Falcon context
 * buffers when the class object is constructed.  Because Kestrel supplies an
 * externally-owned VAS, RM intentionally does not map those buffers into the
 * client page tables.  This is the exact missing half performed by
 * _nvGpuOpsRetainChannelResources() + nvGpuOpsBindChannelResources() in the
 * supplied 595.99.02 Linux driver: query every retained resource, map its
 * physical pages with RM's aperture/kind, commit the VAS, then submit one
 * virtual PROMOTE_CTX and mark the KernelChannel context-bound. */
#define VA_HOST_CTX_BASE   0x00400000000ull
#define VA_HOST_CTX_ALIGN  0x0008000000ull  /* 128 MiB */

static u64 align_up_u64(u64 value, u64 alignment) {
    if (!alignment) return value;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static bool host_ctx_aperture(u32 aperture, bool *vram) {
    if (aperture == 2u) { *vram = true; return true; }   /* ADDR_FBMEM */
    if (aperture == 1u) { *vram = false; return true; } /* ADDR_SYSMEM */
    return false;
}

static bool host_map_gr_resource(nv_card_t *c, nv_rm_t *rm,
                                 nv_channel_t *ch,
                                 const host_gr_ctx_info_t *info, u64 va) {
    bool vram = false;
    if (!info->size || !info->page_size ||
        (info->page_size & (PAGE_SIZE - 1u)) ||
        !host_ctx_aperture(info->aperture, &vram)) {
        kerr("nv-chan", "GR ctx buffer %u has unusable size/page/aperture "
              "(%#llx/%#x/%u)", info->buffer_type,
              (unsigned long long)info->size, info->page_size, info->aperture);
        return false;
    }

    u64 rounded = align_up_u64(info->size, PAGE_SIZE);
    u64 mapped = 0;
    u32 first_page = 0;
    bool read_only = info->buffer_type == GR_BUF_PRIV_ACCESS_MAP ||
                     info->buffer_type == GR_BUF_UNRESTRICTED_PRIV_ACCESS_MAP;

    /* NVIDIA's _shadowMemdescCreate() obtains the physical list through this
     * control even for a contiguous GR buffer; do the same instead of trusting
     * a field that some RM HALs leave informational. */
    while (mapped < rounded) {
        static host_gr_ptes_params_t ptes;
        memset(&ptes, 0, sizeof ptes);
        ptes.h_user_client = rm->client;
        ptes.h_channel = ch->h_channel;
        ptes.buffer_type = info->buffer_type;
        ptes.first_page = first_page;
        u32 got = 0;
        if (!nv_rm_control(c, rm, RM_SUBDEVICE,
                           NV2080_CTRL_CMD_KGR_GET_CTX_BUFFER_PTES,
                           &ptes, sizeof ptes, &ptes, sizeof ptes, &got) ||
            got < 24u || ptes.num_pages == 0 ||
            ptes.num_pages > HOST_GR_PTES_MAX) {
            kerr("nv-chan", "GR ctx buffer %u PTE query stopped at page %u "
                  "(reply %u, status %u)", info->buffer_type, first_page,
                  got, nv_last_control_status);
            return false;
        }
        if (info->is_contiguous) {
            if (!nv_vmm_map_kind(&ch->vmm, va, ptes.phys_addrs[0], rounded,
                                 vram, read_only, true,
                                 info->kind & 0xfu)) {
                kerr("nv-chan", "contiguous GR ctx buffer %u would not map",
                     info->buffer_type);
                return false;
            }
            mapped = rounded;
            break;
        }
        for (u32 i = 0; i < ptes.num_pages && mapped < rounded; i++) {
            u64 run = info->page_size;
            if (run > rounded - mapped) run = rounded - mapped;
            if (!nv_vmm_map_kind(&ch->vmm, va + mapped, ptes.phys_addrs[i],
                                 run, vram, read_only, true,
                                 info->kind & 0xfu)) {
                kerr("nv-chan", "GR ctx buffer %u page %u would not map",
                     info->buffer_type, first_page + i);
                return false;
            }
            mapped += run;
        }
        first_page += ptes.num_pages;
        if (ptes.no_more_pages && mapped < rounded) {
            kerr("nv-chan", "GR ctx buffer %u ended after %#llx of %#llx bytes",
                 info->buffer_type, (unsigned long long)mapped,
                 (unsigned long long)rounded);
            return false;
        }
    }
    return true;
}

static bool host_bind_channel_resources(nv_card_t *c, nv_rm_t *rm,
                                        nv_channel_t *ch, int idx,
                                        u32 engine_type, bool gr) {
    static promote_ctx_params_t promote;
    memset(&promote, 0, sizeof promote);
    promote.engine_type = engine_type;
    promote.h_chan_client = rm->client;
    promote.h_object = ch->h_channel;

    u64 next_va = VA_HOST_CTX_BASE;
    if (gr) {
        static host_gr_ctx_info_params_t q;
        memset(&q, 0, sizeof q);
        q.h_user_client = rm->client;
        q.h_channel = ch->h_channel;
        u32 got = 0;
        if (!nv_rm_control(c, rm, RM_SUBDEVICE,
                           NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO,
                           &q, sizeof q, &q, sizeof q, &got) || got < 16u ||
            q.buffer_count == 0 || q.buffer_count > HOST_GR_CTX_MAX ||
            q.buffer_count > 16u) {
            kerr("nv-chan", "%s: GR context-resource query failed/count %u "
                  "(reply %u, status %u)", ch->name, q.buffer_count, got,
                  nv_last_control_status);
            return false;
        }
        for (u32 i = 0; i < q.buffer_count; i++) {
            const host_gr_ctx_info_t *info = &q.info[i];
            u64 alignment = info->alignment;
            if (alignment < VA_HOST_CTX_ALIGN) alignment = VA_HOST_CTX_ALIGN;
            next_va = align_up_u64(next_va, alignment);
            if (!host_map_gr_resource(c, rm, ch, info, next_va)) return false;
            promote.entry[i].gpu_virt_addr = next_va;
            promote.entry[i].buffer_id = (u16)info->buffer_type;
            kinfo("nv-chan", "%s: mapped RM GR ctx %u at VA %#llx "
                  "(%#llx bytes, page %#x, aperture %u, kind %#x)",
                  ch->name, info->buffer_type,
                  (unsigned long long)next_va,
                  (unsigned long long)info->size, info->page_size,
                  info->aperture, info->kind);
            next_va = align_up_u64(next_va + align_up_u64(info->size, PAGE_SIZE),
                                   VA_HOST_CTX_ALIGN);
        }
        promote.entry_count = q.buffer_count;
    } else {
        static host_flcn_ctx_info_params_t q;
        memset(&q, 0, sizeof q);
        q.h_user_client = rm->client;
        q.h_channel = ch->h_channel;
        /* FLCN_GET_CTX_BUFFER_INFO is SEC2-only in the official 595 RM and
         * therefore necessarily returns NV_ERR_NOT_SUPPORTED for NVDEC/NVENC.
         * Read the exact context memdesc that kflcnAllocContext created through
         * the locked host bridge instead. */
        u8 contiguous = 0, privileged = 0; /* RM NvBool is an unsigned byte. */
        u32 status = nvrm_host_get_video_falcon_context(
            rm->client, ch->h_channel,
            &q.alignment, &q.size, &q.phys_addr,
            &q.aperture, &q.kind, &q.page_size, &contiguous, &privileged);
        q.is_contiguous = contiguous;
        if (status != 0 || !q.size || !q.page_size || !q.is_contiguous) {
            kerr("nv-chan", "%s: video Falcon context-descriptor lookup failed "
                  "(size %#llx, page %#x, contiguous %u, status %u)",
                  ch->name, (unsigned long long)q.size, q.page_size,
                  q.is_contiguous, status);
            return false;
        }
        bool vram = false;
        if (!host_ctx_aperture(q.aperture, &vram)) {
            kerr("nv-chan", "%s: Falcon context has aperture %u",
                 ch->name, q.aperture);
            return false;
        }
        u64 alignment = q.alignment;
        if (alignment < VA_HOST_CTX_ALIGN) alignment = VA_HOST_CTX_ALIGN;
        next_va = align_up_u64(next_va, alignment);
        u64 rounded = align_up_u64(q.size, PAGE_SIZE);
        if (!nv_vmm_map_kind(&ch->vmm, next_va, q.phys_addr, rounded,
                             vram, false, privileged, q.kind & 0xfu)) {
            kerr("nv-chan", "%s: Falcon context would not map", ch->name);
            return false;
        }
        promote.entry[0].gpu_virt_addr = next_va;
        promote.entry_count = 1;
        kinfo("nv-chan", "%s: mapped RM Falcon ctx at VA %#llx "
              "(%#llx bytes, page %#x, aperture %u, kind %#x, RM privileged=%u)",
              ch->name, (unsigned long long)next_va,
              (unsigned long long)q.size, q.page_size, q.aperture, q.kind, privileged);
    }

    if (!nv_vmm_commit(c, rm, &ch->vmm, h_vaspace(idx),
                       gr ? "host GR context" : "host Falcon context"))
        return false;
    if (!nv_rm_control(c, rm, RM_SUBDEVICE,
                       NV2080_CTRL_CMD_GPU_PROMOTE_CTX,
                       &promote, sizeof promote, NULL, 0, NULL)) {
        kerr("nv-chan", "%s: virtual PROMOTE_CTX of %u RM resource(s) "
              "was refused (%u)", ch->name, promote.entry_count,
              nv_last_control_status);
        return false;
    }
    u32 status = nvrm_host_mark_context_bound(rm->client, ch->h_channel);
    if (status != 0) {
        kerr("nv-chan", "%s: RM KernelChannel context-bound transition "
              "failed (%u)", ch->name, status);
        return false;
    }
    kinfo("nv-chan", "%s: %u RM context resource(s) mapped, promoted and bound",
          ch->name, promote.entry_count);
    return true;
}

/* Pull GSP-RM's writes back into the CPU shadow.  Later nv_vmm_map() calls
 * rewrite whole table pages through nv_vmm_push_to_vram(); without this sync
 * they would erase the reserved PDE that the control just installed. */
static bool nv_vmm_pull_from_vram(nv_card_t *c, nv_rm_t *rm, nv_vmm_t *vmm) {
    u64 bytes = (u64)vmm->used * PAGE_SIZE;
    if (rm->host_api) {
        u32 memory = nv_vmm_memory_handle(vmm);
        if (!memory) return false;
        u32 status = nvrm_transfer_rm_memory(rm->client, memory, 0,
                                              vmm->pool, bytes, true);
        if (status != 0) {
            kerr("nv-chan", "coherent host-RM page-table readback failed (%u)", status);
            return false;
        }
        return true;
    }
    for (u32 o = 0; o < vmm->used * PAGE_SIZE; o += 4)
        *(u32 *)(vmm->pool + o) = nv_fb_rd32(c, vmm->pool_gpu + o);
    return true;
}

/* Bind the page-table levels for GSP-RM's mandatory split-VAS window.  Host
 * RM normally performs this while constructing a client VASpace; Kestrel talks
 * to server RM directly, so merely allocating FERMI_VASPACE_A and calling
 * SET_PAGE_DIRECTORY leaves 4 GiB..4.5 GiB absent. */
static bool nv_vaspace_copy_server_reserved_pdes(nv_card_t *c, nv_rm_t *rm,
                                                  nv_vmm_t *vmm, u32 h_vas,
                                                  const char *name) {
    server_reserved_pdes_params_t p;
    memset(&p, 0, sizeof p);
    p.page_size = SPLIT_VAS_SERVER_RM_MANAGED_VA_SIZE;
    p.virt_addr_lo = SPLIT_VAS_SERVER_RM_MANAGED_VA_START;
    p.virt_addr_hi = p.virt_addr_lo + p.page_size - 1;
    p.num_levels_to_copy = 4;
    if (!nv_vmm_reserve_512m_levels(vmm, p.virt_addr_lo, p.levels)) {
        kwarn("nv-chan", "%s: could not reserve the server-RM PDE path", name);
        return false;
    }

    /* The path created above exists only in the CPU shadow until pushed. */
    if (!nv_vmm_push_to_vram(c, rm, vmm)) return false;
    bool ok = nv_rm_control(c, rm, h_vas,
                            NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES,
                            &p, sizeof p, NULL, 0, NULL);
    kinfo("nv-chan", "%s: COPY_SERVER_RESERVED_PDES 4G..4.5G (%u levels) -> %s",
          name, p.num_levels_to_copy, ok ? "accepted" : "REFUSED");
    if (!ok) return false;

    /* Preserve the entry/entries GSP-RM wrote when subsequent channel mappings
     * are added to this same tree. */
    if (!nv_vmm_pull_from_vram(c, rm, vmm)) return false;
    return true;
}

/* Describe the GR context buffers from the device (fills g_ctxbuf[]).  Nouveau
 * targets a PRIVILEGED internal subdevice for this control; whether GSP-RM
 * enforces that (vs accepting our ordinary RM_SUBDEVICE) is NOT provable from
 * source, so PROBE RM_SUBDEVICE first and log the outcome.  If it is refused the
 * next step is RPC 65 (GET_GSP_STATIC_INFO) to obtain hInternalSubdevice and
 * retarget - not implemented here; the boot log will say whether it is needed. */
static bool nv_gr_query_ctxbufs(nv_card_t *c, nv_rm_t *rm) {
    static gr_ctxbufs_params_t p;
    memset(&p, 0, sizeof p);
    u32 got = 0;
    if (!nv_rm_control_prefer_internal(c, rm, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_CTXBUFS_INFO,
                       &p, sizeof p, &p, sizeof p, &got) || got < 8) {
        kwarn("nv-chan", "GR ctxbuf-info failed on both our subdevice and GSP-RM's "
              "internal one (got %u). Golden context cannot proceed this boot.", got);
        return false;
    }
    g_ctxbuf_nr = 0;
    for (u32 i = 0; i < GR_ENGINE_ID_COUNT; i++) {
        u32 raw = p.engine[0][i].size;
        for (unsigned m = 0; m < sizeof gr_ctxbuf_map / sizeof gr_ctxbuf_map[0]; m++) {
            if (gr_ctxbuf_map[m].id0 != i) continue;
            if (raw == 0) { kwarn("nv-chan", "  ctxbuf id0 %#x size 0 - skipped", i); break; }
            u32 sz = raw;
            if (gr_ctxbuf_map[m].buffer_id == GR_BUF_MAIN)   /* +per-subctx headers */
                sz = ((sz + 0xfffu) & ~0xfffu) + 64u * 0x1000u;
            u8 page = sz >= (1u << 21) ? 21 : sz >= (1u << 16) ? 16 : 12;
            gr_ctxbuf_desc_t *d = &g_ctxbuf[g_ctxbuf_nr++];
            d->buffer_id = gr_ctxbuf_map[m].buffer_id; d->size = sz; d->page = page;
            d->global = gr_ctxbuf_map[m].global; d->init = gr_ctxbuf_map[m].init;
            d->ro = gr_ctxbuf_map[m].ro; d->golden_fb = 0;
            (void)order_base_2_u32;   /* align tracked by page; VA base is 128 MiB aligned */
            kinfo("nv-chan", "  ctxbuf[%d] id0 %#x -> bufId %u size %#x page %u %s%s%s",
                  g_ctxbuf_nr - 1, i, d->buffer_id, d->size, page,
                  d->global ? "global " : "perchan ", d->init ? "init " : "", d->ro ? "ro" : "");
            if (gr_ctxbuf_map[m].buffer_id == GR_BUF_PRIV_ACCESS_MAP && g_ctxbuf_nr < 16) {
                gr_ctxbuf_desc_t *u = &g_ctxbuf[g_ctxbuf_nr++];
                *u = *d; u->buffer_id = GR_BUF_UNRESTRICTED_PRIV_ACCESS_MAP;
            }
            break;
        }
    }
    kinfo("nv-chan", "GR context: %d buffer(s) described from the device", g_ctxbuf_nr);
    return g_ctxbuf_nr > 0;
}

/* Promote the GR context into a channel.  golden=true (the one-time golden
 * channel) allocates every buffer fresh and remembers the GLOBAL ones for reuse;
 * golden=false (a real channel) allocates only MAIN/PATCH fresh and reuses the
 * globals.  Faithful to r535_gr_promote_ctx (gr.c:59-140). */
static bool nv_gr_promote(nv_card_t *c, nv_rm_t *rm, bool golden,
                          nv_vmm_t *vmm, u32 h_chan, u32 vram_base) {
    static promote_ctx_params_t p;
    memset(&p, 0, sizeof p);
    p.engine_type   = 1;                 /* GRAPHICS, always */
    p.h_chan_client = rm->client;
    p.h_object      = h_chan;
    u64 va = 0x00400000000ull;           /* ctxbuf VA window in this vmm (128 MiB stride) */
    for (int i = 0; i < g_ctxbuf_nr && p.entry_count < 16; i++) {
        gr_ctxbuf_desc_t *cb = &g_ctxbuf[i];
        bool alloc = golden || !cb->global;
        promote_entry_t *e = &p.entry[p.entry_count];
        u64 fb = 0; bool nonmapped = false;
        e->buffer_id    = cb->buffer_id;
        e->b_initialize = (cb->init && alloc) ? 1 : 0;
        if (alloc) {
            if (!nv_vram_alloc(c, rm, vram_base + (u32)i, cb->size, &fb)) {
                kwarn("nv-chan", "GR promote: no VRAM for buffer %u (%#x bytes)", cb->buffer_id, cb->size);
                return false;
            }
            if (golden && cb->global) cb->golden_fb = fb;   /* keep for real channels */
            if (cb->buffer_id == GR_BUF_PRIV_ACCESS_MAP) { e->b_nonmapped = 1; nonmapped = true; }
        } else {
            if (cb->buffer_id == GR_BUF_UNRESTRICTED_PRIV_ACCESS_MAP) continue; /* golden-only */
            fb = cb->golden_fb;
            if (!fb) { kwarn("nv-chan", "GR promote: global buffer %u has no golden allocation", cb->buffer_id); return false; }
        }
        if (!nonmapped) {
            u64 bytes = ((u64)cb->size + PAGE_SIZE - 1) & ~((u64)PAGE_SIZE - 1);
            /* priv=true: GR context-switch buffers MUST be privileged PTEs or
             * FECS/GPCCS faults GR with GR_EXCEPTION on the first grid (no MMU
             * fault - the mapping is valid, just the wrong PCF).  Matches
             * nouveau r535_gr_promote_ctx's .priv=1 on every ctxbuf. */
            if (!nv_vmm_map(vmm, va, fb, bytes, true, cb->ro != 0, true)) {
                kwarn("nv-chan", "GR promote: could not map buffer %u (%llu bytes) into the vaspace",
                      cb->buffer_id, (unsigned long long)bytes);
                return false;
            }
            e->gpu_virt_addr = va;
            va += 0x08000000ull;         /* 128 MiB apart, comfortably clear of each other */
        }
        if (e->b_initialize) { e->gpu_phys_addr = fb; e->size = cb->size; e->phys_attr = 4; }
        kinfo("nv-chan", "  promote %s buf %u: fb %#llx va %#llx init %u nm %u",
              golden ? "golden" : "chan", cb->buffer_id, (unsigned long long)fb,
              (unsigned long long)e->gpu_virt_addr, e->b_initialize, e->b_nonmapped);
        p.entry_count++;
    }
    /* The ctxbuf mappings we just added must reach VRAM and the live VAS must
     * discard any cached translations before RM/host can walk them. */
    int idx = -1;
    for (int j = 0; j < CH_COUNT; j++)
        if (&channels[j].vmm == vmm) { idx = j; break; }
    if (idx < 0 || !nv_vmm_commit(c, rm, vmm, h_vaspace(idx),
                                  "direct GR context"))
        return false;
    bool ok = nv_rm_control(c, rm, RM_SUBDEVICE, NV2080_CTRL_CMD_GPU_PROMOTE_CTX,
                            &p, sizeof p, NULL, 0, NULL);
    kinfo("nv-chan", "GR PROMOTE_CTX(golden=%d, %u entries) -> %s",
          golden, p.entry_count, ok ? "accepted" : "REFUSED");
    return ok;
}

/* tinygrad's external-client compute path differs from nouveau's combined
 * channel promotion in two precise ways:
 *
 *   - it adds a per-channel PM ctxsw buffer (buffer ID 1), sized like PATCH;
 *   - it promotes MAIN/PM/PATCH in two calls: physical initialization first
 *     (bNonmapped=1), then their virtual addresses with no physical payload.
 *
 * Keep this experiment compute-only.  Golden capture and 3D continue through
 * nv_gr_promote(), so a hardware run cleanly tells us whether this external-
 * client convention is the missing compute dependency. */
static bool nv_gr_promote_compute_twophase(nv_card_t *c, nv_rm_t *rm,
                                            nv_vmm_t *vmm, u32 h_chan,
                                            u32 vram_base) {
    gr_ctxbuf_desc_t *main = NULL, *patch = NULL;
    for (int i = 0; i < g_ctxbuf_nr; i++) {
        if (g_ctxbuf[i].buffer_id == GR_BUF_MAIN) main = &g_ctxbuf[i];
        if (g_ctxbuf[i].buffer_id == GR_BUF_PATCH) patch = &g_ctxbuf[i];
    }
    if (!main || !patch) {
        kwarn("nv-chan", "compute two-phase: MAIN/PATCH descriptions are missing");
        return false;
    }

    const u16 ids[3] = { GR_BUF_MAIN, GR_BUF_PM, GR_BUF_PATCH };
    const gr_ctxbuf_desc_t *desc[3] = { main, patch, patch };
    u64 fb[3] = { 0, 0, 0 };
    u64 va[3] = { 0x00400000000ull, 0x00408000000ull, 0x00410000000ull };

    for (u32 i = 0; i < 3; i++) {
        if (!nv_vram_alloc(c, rm, vram_base + i, desc[i]->size, &fb[i])) {
            kwarn("nv-chan", "compute two-phase: no VRAM for buffer %u (%#x bytes)",
                  ids[i], desc[i]->size);
            return false;
        }
        u64 bytes = ((u64)desc[i]->size + PAGE_SIZE - 1) & ~((u64)PAGE_SIZE - 1);
        if (!nv_vmm_map(vmm, va[i], fb[i], bytes, true, false, true)) {
            kwarn("nv-chan", "compute two-phase: could not map buffer %u", ids[i]);
            return false;
        }
        kinfo("nv-chan", "  compute ctx buf %u: fb %#llx va %#llx size %#x",
              ids[i], (unsigned long long)fb[i], (unsigned long long)va[i],
              desc[i]->size);
    }
    int idx = -1;
    for (int j = 0; j < CH_COUNT; j++)
        if (&channels[j].vmm == vmm) { idx = j; break; }
    if (idx < 0 || !nv_vmm_commit(c, rm, vmm, h_vaspace(idx),
                                  "two-phase GR context"))
        return false;

    static promote_ctx_params_t p;
    memset(&p, 0, sizeof p);
    p.engine_type = NV2080_ENGINE_TYPE_GRAPHICS;
    p.h_chan_client = rm->client;
    p.h_object = h_chan;
    p.entry_count = 3;
    for (u32 i = 0; i < 3; i++) {
        p.entry[i].gpu_phys_addr = fb[i];
        p.entry[i].size = desc[i]->size;
        p.entry[i].phys_attr = 4;
        p.entry[i].buffer_id = ids[i];
        p.entry[i].b_initialize = 1;
        p.entry[i].b_nonmapped = 1;
    }
    bool phys_ok = nv_rm_control(c, rm, RM_SUBDEVICE,
                                 NV2080_CTRL_CMD_GPU_PROMOTE_CTX,
                                 &p, sizeof p, NULL, 0, NULL);
    kinfo("nv-chan", "compute PROMOTE_CTX phase 1/physical (MAIN+PM+PATCH) -> %s",
          phys_ok ? "accepted" : "REFUSED");
    if (!phys_ok) return false;

    memset(&p, 0, sizeof p);
    p.engine_type = NV2080_ENGINE_TYPE_GRAPHICS;
    p.h_chan_client = rm->client;
    p.h_object = h_chan;
    p.entry_count = 3;
    for (u32 i = 0; i < 3; i++) {
        p.entry[i].gpu_virt_addr = va[i];
        p.entry[i].buffer_id = ids[i];
    }
    bool virt_ok = nv_rm_control(c, rm, RM_SUBDEVICE,
                                 NV2080_CTRL_CMD_GPU_PROMOTE_CTX,
                                 &p, sizeof p, NULL, 0, NULL);
    kinfo("nv-chan", "compute PROMOTE_CTX phase 2/virtual (MAIN+PM+PATCH) -> %s",
          virt_ok ? "accepted" : "REFUSED");
    return virt_ok;
}

/* ---- NVDEC/NVENC falcon context ----
 * A falcon engine (NVDEC/NVENC) needs a context promoted before it runs, unlike
 * the copy engine.  Unlike GR's golden image this is ONE per-channel ctx buffer
 * whose size the device reports (GET_CONSTRUCTED_FALCON_INFO, keyed by the
 * engine's device-info engDesc); the promote passes virtAddress+size directly
 * with entryCount=0 and engineType = the public NV2080 id.  Faithful to nouveau
 * r535_flcn_ctor + r535_flcn_bind (r535/fifo.c:277-320,427-454). */
#define NV2080_CTRL_CMD_GPU_GET_CONSTRUCTED_FALCON_INFO 0x208001b0u  /* r570 (570.144) */
#define FALCON_MAX 0x40u
#define ENGINE_INFO_TYPE_ENG_DESC 0u
typedef struct { u32 eng_desc, ctx_attr, ctx_buffer_size, addr_space_list, register_base; }
    __attribute__((packed)) falcon_info_t;
_Static_assert(sizeof(falcon_info_t) == 20, "constructed-falcon entry is 20 bytes");
typedef struct { u32 num; falcon_info_t table[FALCON_MAX]; }
    __attribute__((packed)) falcon_info_params_t;

/* The device-info ENG_DESC for an engine (the key into the falcon table). */
static bool nv_query_engdesc(nv_card_t *c, nv_rm_t *rm, u32 engine_type, u32 *out) {
    static dev_info_table_t t; memset(&t, 0, sizeof t); u32 got = 0;
    u32 want = nv2080_to_rm_engine_type(engine_type);
    if (!nv_rm_control(c, rm, RM_SUBDEVICE, NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE,
                       &t, sizeof t, &t, sizeof t, &got) || got < 8) return false;
    u32 n = t.num_entries; if (n > DEV_INFO_MAX_ENTRIES) n = DEV_INFO_MAX_ENTRIES;
    for (u32 i = 0; i < n; i++)
        if (t.entries[i].engine_data[ENGINE_INFO_TYPE_RM_ENGINE_TYPE] == want) {
            *out = t.entries[i].engine_data[ENGINE_INFO_TYPE_ENG_DESC];
            return true;
        }
    return false;
}

/* Falcon ctx-buffer size for engine_type, matched by engDesc. */
static bool nv_flcn_ctx_size(nv_card_t *c, nv_rm_t *rm, u32 engine_type, u32 *out_size) {
    u32 engdesc = 0;
    if (!nv_query_engdesc(c, rm, engine_type, &engdesc)) {
        kwarn("nv-chan", "falcon: no engDesc for engine %u in device-info table", engine_type);
        return false;
    }
    static falcon_info_params_t f; memset(&f, 0, sizeof f); u32 got = 0;
    if (!nv_rm_control_internal(c, rm, NV2080_CTRL_CMD_GPU_GET_CONSTRUCTED_FALCON_INFO,
                       &f, sizeof f, &f, sizeof f, &got) || got < 4) {
        kwarn("nv-chan", "falcon: GET_CONSTRUCTED_FALCON_INFO refused on both our and "
                         "the internal subdevice");
        return false;
    }
    u32 n = f.num; if (n > FALCON_MAX) n = FALCON_MAX;
    for (u32 i = 0; i < n; i++)
        if (f.table[i].eng_desc == engdesc) {
            *out_size = f.table[i].ctx_buffer_size;
            kinfo("nv-chan", "falcon: ctx buffer size %u (engDesc %#x)", *out_size, engdesc);
            return *out_size > 0;
        }
    kwarn("nv-chan", "falcon: engDesc %#x not found in the constructed-falcon table", engdesc);
    return false;
}

/* Promote a Falcon (NVDEC/NVENC) context into our externally-owned VAS exactly
 * like Linux r570/r535 Nouveau's r535_flcn_bind().  That client owns both the
 * context VRAM and page tables, so it sends one top-level virtAddress+size
 * request after mapping the buffer.  NVIDIA's internal kernel_falcon path uses
 * a different retained-resource API with separate physical and virtual entry
 * calls; mixing that internal protocol into this direct-GSP client would
 * double-register a context.  The crucial correction from the failed 13:35
 * build is engine_type: it is the public NV2080 id (Nouveau engn->id), never
 * the internal RM engine descriptor. */
#define VA_FLCN_CTX 0x00300000000ull
static bool nv_flcn_promote(nv_card_t *c, nv_rm_t *rm, nv_channel_t *ch,
                            u32 engine_type, u32 h_channel_handle, u32 vram_handle) {
    u32 size = 0;
    if (!nv_flcn_ctx_size(c, rm, engine_type, &size)) return false;
    u64 fb = 0;
    if (!nv_vram_alloc(c, rm, vram_handle, size, &fb)) return false;
    u64 bytes = ((u64)size + PAGE_SIZE - 1) & ~((u64)PAGE_SIZE - 1);
    if (!nv_vmm_map(&ch->vmm, VA_FLCN_CTX, fb, bytes, true, false, false)) {
        kwarn("nv-chan", "falcon: could not map the ctx buffer into the vaspace");
        return false;
    }
    int idx = nv_channel_index(ch);
    if (idx < 0 || !nv_vmm_commit(c, rm, &ch->vmm, h_vaspace(idx),
                                  "Falcon context"))
        return false;
    static promote_ctx_params_t p; memset(&p, 0, sizeof p);
    p.engine_type   = engine_type;
    p.h_client      = rm->client;
    p.h_chan_client = rm->client;
    p.h_object      = h_channel_handle;
    p.ch_id         = ch->chid;
    p.virt_address  = VA_FLCN_CTX;
    p.size          = size;
    p.entry_count   = 0u;
    bool ok = nv_rm_control(c, rm, RM_SUBDEVICE,
                            NV2080_CTRL_CMD_GPU_PROMOTE_CTX,
                            &p, sizeof p, NULL, 0, NULL);
    kinfo("nv-chan", "falcon PROMOTE_CTX external-VAS VA+size (FB %#llx VA %#llx, size %u, NV2080 engine %#x) -> %s",
          (unsigned long long)fb, (unsigned long long)VA_FLCN_CTX,
          size, engine_type, ok ? "accepted" : "REFUSED");
    return ok;
}

/* Bring up one engine channel: pool + page tables + VA space + page directory
 * + TSG + context share + GPFIFO channel + primary engine object + bind +
 * schedule.  Every step logs, so a failure names exactly which RM alloc the
 * card refused.  Returns 0 on a fully open, scheduled channel. */
/* Diagnostic parameter sweep for the persistent channel-alloc
 * INVALID_PARAMETER(59).  Every field of our NV_CHANNEL_ALLOC_PARAMS has been
 * verified byte-for-byte against nouveau's known-working r535/r570 path, so the
 * 59 is raised by a GSP-RM callee we cannot see from source.  Rather than burn a
 * boot per guess, try a matrix of single-field variations in ONE boot and log
 * the exact status each returns.  The template `base` is a fully-built params
 * block (valid vaspace, VRAM instance/ramfc, sysmem method buffer); each trial
 * copies it and changes exactly one thing.  If a variation succeeds we adopt it
 * (return its handle + engineType) so the pipeline can proceed. */
static bool nv_chan_diag_sweep(nv_card_t *c, nv_rm_t *rm,
                               const channel_alloc_params_t *base,
                               const char *name,
                               u32 *out_handle, u32 *out_engine,
                               int *out_userd_vidmem) {
    struct { const char *desc; u32 engine; int userd_vidmem; } trials[] = {
        { "engineType=COPY4(13)",        0x0du, 0 },
        { "engineType=COPY5(14)",        0x0eu, 0 },
        { "engineType=GRAPHICS(1)",      0x01u, 0 },
        { "engineType=NULL(0)",          0x00u, 0 },
        { "userd VIDMEM, COPY0(9)",      0x09u, 1 },
        { "userd VIDMEM + COPY4(13)",    0x0du, 1 },
    };
    kinfo("nv-chan", "%s: sweeping channel-alloc variations to isolate the 59", name);
    for (unsigned t = 0; t < sizeof trials / sizeof trials[0]; t++) {
        channel_alloc_params_t v = *base;
        v.engine_type = trials[t].engine;
        u32 handle = 0xCAFE0000u + t;
        if (trials[t].userd_vidmem) {
            u64 userd_fb = 0;
            if (!nv_vram_alloc(c, rm, 0x004F0000u + t, 0x1000, &userd_fb)) {
                kwarn("nv-chan", "  %-26s : no VRAM for userd, skipped", trials[t].desc);
                continue;
            }
            v.userd_mem.base = userd_fb;
            v.userd_mem.address_space = ADDR_SPACE_VIDMEM;
            v.userd_mem.cache_attrib = 1;
        }
        bool ok = nv_rm_alloc(c, rm, RM_DEVICE, handle, BLACKWELL_CHANNEL_GPFIFO_B,
                              &v, sizeof v);
        kinfo("nv-chan", "  %-26s -> %s (%u)", trials[t].desc,
              ok ? "SUCCESS" : "refused", nv_last_alloc_status);
        if (ok) {
            *out_handle = handle;
            *out_engine = trials[t].engine;
            *out_userd_vidmem = trials[t].userd_vidmem;
            return true;
        }
    }
    kinfo("nv-chan", "%s: no variation opened the channel; the 59 is not any swept field",
          name);
    return false;
}

/* Second diagnostic: is the 59 the page-directory APERTURE?  Our page tables
 * live in sysmem (SET_PAGE_DIRECTORY SYSMEM_COH); nouveau's known-working r535
 * path keeps the whole tree in VRAM (SET_PAGE_DIRECTORY VIDMEM).  That is the
 * last structural divergence, and the sweep proved the 59 is param-independent -
 * exactly what a bad precondition (the vaspace) looks like.  Build a fresh
 * externally-owned vaspace whose page directory is a VRAM page (VIDMEM aperture)
 * and try ONE channel against it.  The VRAM directory is unpopulated - enough to
 * tell whether RM accepts a channel when the directory is in VRAM.  If this
 * SUCCEEDS where the sysmem directory is refused, the aperture is the cause and
 * the fix is real VRAM page tables (writable via the PRAMIN/BAR0 window). */
static void nv_chan_diag_vram_pd(nv_card_t *c, nv_rm_t *rm,
                                 const channel_alloc_params_t *base,
                                 const char *name) {
    const u32 h_vas = 0x90f1ee00u;   /* distinct from every h_vaspace(idx) */
    const u32 h_ch  = 0xCAFEDD00u;
    u64 pd_fb = 0;
    if (!nv_vram_alloc(c, rm, 0x004FDD00u, 0x1000, &pd_fb)) {
        kwarn("nv-chan", "%s: VRAM-PD test: no VRAM for the directory", name); return;
    }
    vaspace_params_t vas = { 0 };
    vas.index = 0;
    vas.flags = (1u << 3);                        /* IS_EXTERNALLY_OWNED */
    if (!nv_rm_alloc(c, rm, RM_DEVICE, h_vas, FERMI_VASPACE_A, &vas, sizeof vas)) {
        kwarn("nv-chan", "%s: VRAM-PD test: second vaspace refused (%u)",
              name, nv_last_alloc_status);
        return;
    }
    set_page_dir_params_t pd = { 0 };
    pd.phys_address = pd_fb;
    pd.num_entries  = 2;
    pd.flags        = 0;                           /* APERTURE_VIDMEM (0) */
    pd.h_vaspace    = h_vas;
    bool pd_ok = nv_rm_control(c, rm, RM_DEVICE, NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY,
                               &pd, sizeof pd, NULL, 0, NULL);
    kinfo("nv-chan", "%s: VRAM-PD test: SET_PAGE_DIRECTORY(VIDMEM) -> %s",
          name, pd_ok ? "accepted" : "refused");
    if (!pd_ok) return;
    channel_alloc_params_t v = *base;
    v.h_vaspace   = h_vas;
    v.engine_type = 0x09u;                         /* COPY0 */
    bool ok = nv_rm_alloc(c, rm, RM_DEVICE, h_ch, BLACKWELL_CHANNEL_GPFIFO_B,
                          &v, sizeof v);
    kinfo("nv-chan", "%s: VRAM-PD test: channel with a VRAM page directory -> %s (%u)",
          name, ok ? "SUCCESS - the aperture WAS the 59" : "refused", nv_last_alloc_status);
}

/* Third diagnostic: is the 59 the auto-wrapped channel GROUP (TSG)?  A channel
 * allocated directly under the device makes GSP-RM auto-create a
 * KEPLER_CHANNEL_GROUP_A to wrap it (kernel_channel.c construct L359-412).  If
 * THAT internal alloc is what fails, the 59 is the runlist/TSG, not the channel.
 * Allocate the TSG explicitly and log it; if it succeeds, allocate a channel
 * UNDER the TSG (hVASpace MUST be 0 then - a TSG owns the vaspace). */
static void nv_chan_diag_tsg(nv_card_t *c, nv_rm_t *rm,
                             const channel_alloc_params_t *base, const char *name) {
    const u32 h_grp = 0xa06cee00u;
    const u32 h_ch  = 0xCAFEC600u;
    tsg_params_t tp = { 0 };
    tp.h_vaspace   = base->h_vaspace;
    tp.engine_type = 0x09u;                        /* COPY0 */
    bool grp_ok = nv_rm_alloc(c, rm, RM_DEVICE, h_grp, KEPLER_CHANNEL_GROUP_A,
                              &tp, sizeof tp);
    kinfo("nv-chan", "%s: TSG test: KEPLER_CHANNEL_GROUP_A -> %s (%u)",
          name, grp_ok ? "SUCCESS" : "refused", nv_last_alloc_status);
    if (!grp_ok) return;
    channel_alloc_params_t v = *base;
    v.engine_type = 0x09u;
    v.h_vaspace   = 0;                             /* the TSG carries the vaspace */
    bool ok = nv_rm_alloc(c, rm, h_grp, h_ch, BLACKWELL_CHANNEL_GPFIFO_B,
                          &v, sizeof v);
    kinfo("nv-chan", "%s: TSG test: channel under the TSG -> %s (%u)",
          name, ok ? "SUCCESS - the auto-wrap WAS the problem" : "refused",
          nv_last_alloc_status);
}

/* golden=true opens a throwaway GR channel with NO engine object (used to
 * capture the golden context); every real channel passes golden=false. */
static int open_engine_channel(nv_card_t *c, nv_rm_t *rm, int idx,
                               u32 engine_type, u32 obj_class,
                               const void *obj_params, u32 obj_params_size,
                               const char *name, bool golden) {
    nv_channel_t *ch = &channels[idx];
    memset(ch, 0, sizeof *ch);
    ch->card = c;
    ch->rm = rm;
    ch->chid = (u32)idx;
    ch->name = name;
    ch->obj_class = obj_class;
    bool gr_class = (obj_class == BLACKWELL_B || obj_class == BLACKWELL_COMPUTE_B);

    u64 phys = 0;
    ch->pool_va = dma_alloc_pages(POOL_BYTES / PAGE_SIZE, &phys);
    if (!ch->pool_va) { kerr("nv-chan", "%s: no sysmem pool", name); return -1; }
    ch->pool_phys = phys;
    memset(ch->pool_va, 0, POOL_BYTES);

    /* Blackwell uses the Turing host submission ordering in NVIDIA's current
     * Linux UVM driver (uvm_turing_host.c): when GP_PUT is in sysmem, issue a
     * dummy BAR1 read before publishing GP_PUT.  The value is irrelevant (and
     * may be poison while RM owns BAR1); completion of the PCIe read is the
     * ordering primitive.  Keep one mapped dword per channel so the hot submit
     * path exactly follows that contract without remapping anything. */
    if (rm->host_api && c->vram_base && c->vram_aperture)
        ch->submit_bar1_flush = (volatile u32 *)vmm_map_mmio(c->vram_base, 4);

    const u32 tables_bytes = POOL_BYTES / 2;
    u32 off = tables_bytes;
    u32 gpfifo_off   = off; off += GPFIFO_BYTES;
    u32 userd_off    = off; off += USERD_BYTES;
    u32 pushbuf_off  = off; off += PUSHBUF_BYTES;
    u32 sem_off      = off; off += SEM_BYTES;
    /* The method buffer stays in sysmem (nouveau puts mthdbufMem in sysmem too).
     * The instance block and RAMFC are NOT carved here any more: on a GSP-client
     * card RM requires them in VRAM, so they are allocated with nv_vram_alloc
     * below. */
    u32 mthdbuf_off  = off; off += MTHDBUF_BYTES;
    if (off > POOL_BYTES) { kerr("nv-chan", "%s: pool too small", name); return -1; }

    ch->gpfifo  = (volatile u32 *)((u8 *)ch->pool_va + gpfifo_off);
    ch->userd   = (volatile u32 *)((u8 *)ch->pool_va + userd_off);
    ch->pushbuf = (volatile u8  *)((u8 *)ch->pool_va + pushbuf_off);
    ch->sem     = (volatile u32 *)((u8 *)ch->pool_va + sem_off);
    ch->pb_at = PB_METHOD_BEGIN;
    ch->gpfifo_phys  = pool_phys_at(ch, gpfifo_off);
    ch->userd_phys   = pool_phys_at(ch, userd_off);
    ch->pushbuf_phys = pool_phys_at(ch, pushbuf_off);
    ch->sem_phys     = pool_phys_at(ch, sem_off);

    /* The page-table TREE must live in VRAM: the GB20x host/PBDMA will not walk a
     * channel page directory that sits in sysmem (RM accepts a sysmem root at
     * SET_PAGE_DIRECTORY, but the channel is then never made runnable and the
     * host never fetches - GP_GET stays 0 with no MMU fault, exactly the symptom
     * we saw).  Every real GSP client keeps the tree in VRAM.  So: build the
     * tables in a sysmem SHADOW (ch->pool_va, CPU-writable) but give them VRAM
     * addresses (pool_gpu = a VRAM allocation), then copy the shadow into VRAM
     * through the BAR0 window before handing RM the VRAM root.  Leaf data pages
     * (ring/pushbuffer/semaphore) stay in sysmem - only the tree moves. */
    u64 tables_fb = 0;
    if (!nv_vram_alloc(c, rm, h_tablesvram(idx), tables_bytes, &tables_fb)) {
        kerr("nv-chan", "%s: no VRAM for the page tables", name); return -1;
    }
    if (!nv_vmm_init(&ch->vmm, (u8 *)ch->pool_va, tables_fb,
                     tables_bytes / PAGE_SIZE, true)) {
        kerr("nv-chan", "%s: page tables would not initialise", name);
        return -1;
    }
    if (!nv_vmm_map(&ch->vmm, VA_GPFIFO,  ch->gpfifo_phys,  GPFIFO_BYTES,  false, false, false) ||
        !nv_vmm_map(&ch->vmm, VA_PUSHBUF, ch->pushbuf_phys, PUSHBUF_BYTES, false, false, false) ||
        !nv_vmm_map(&ch->vmm, VA_SEM,     ch->sem_phys,     SEM_BYTES,     false, false, false)) {
        kerr("nv-chan", "%s: could not map ring/pushbuffer into the VA space", name);
        return -1;
    }
    if (!nv_vmm_push_to_vram(c, rm, &ch->vmm)) {
        kerr("nv-chan", "%s: initial page-table upload failed", name);
        return -1;
    }
    kinfo("nv-chan", "%s: %llu bytes of page tables uploaded coherently",
          name, (unsigned long long)ch->vmm.used * PAGE_SIZE);

    /* 1. Golden uses an RM-managed VASpace, matching Nouveau/tinygrad.  Real
     * channels remain externally owned and receive our root below. */
    vaspace_params_t vas = { 0 };
    vas.index = 0;
    vas.flags = golden ? 0 : (1u << 3);               /* IS_EXTERNALLY_OWNED   */
    if (!nv_rm_alloc(c, rm, RM_DEVICE, h_vaspace(idx), FERMI_VASPACE_A, &vas, sizeof vas)) {
        kerr("nv-chan", "%s: the VA space could not be allocated", name); return -1;
    }

    /* A direct GSP client has no host RM to splice server-RM's reserved range
     * into its tables.  Exercise that correction only on GR VASpaces in this
     * controlled build; the already-proven copy channel and unrelated falcon
     * channels keep their prior setup so a rejected new control cannot hide
     * the baseline. */
    if ((golden || gr_class) &&
        !nv_vaspace_copy_server_reserved_pdes(c, rm, &ch->vmm,
                                               h_vaspace(idx), name)) {
        kerr("nv-chan", "%s: server-RM reserved PDE setup failed", name);
        return -1;
    }

    /* 2. External clients hand RM our page-directory root.  COPY_SERVER_* is
     * the corresponding registration for the RM-managed golden VASpace, so it
     * must not also receive SET_PAGE_DIRECTORY. */
    if (!golden) {
        set_page_dir_params_t pd = { 0 };
        pd.phys_address = ch->vmm.root_gpu;
        /* numEntries is the count of PDEs in the TOP level of the page directory.
         * Blackwell's GMMU is the Hopper 5-level tree over a 57-bit VA
         * (12 page + 9 + 8 + 9 + 9 + 9 + 1): the ROOT level indexes with just ONE
         * bit, so it has exactly 2 entries - NOT 4.  (Confirmed vs nouveau
         * vmmgh100: page[0].desc is gh100_vmm_desc_16[5] = {PGD, 1, ...}, and it
         * sends numEntries = 1 << 1 = 2.)  RM's own SET_PAGE_DIRECTORY handler
         * (gvaspaceExternalRootDirCommit) computes the VA the directory covers as
         * mmuFmtEntryIndexVirtAddrHi(root, 0, numEntries-1) and rejects it with
         * INVALID_ARGUMENT (31) when it exceeds vaLimitMax - which is why 512 AND 4
         * both failed: 4 entries overshoot a 2-entry root's 57-bit span.  Two is the
         * only value that covers exactly [0, vaLimitMax]. */
        pd.num_entries = 2;
        pd.flags = 0;   /* APERTURE_VIDMEM: the root (and whole tree) is now in VRAM */
        pd.h_vaspace = h_vaspace(idx);
        if (!nv_rm_control(c, rm, RM_DEVICE, NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY,
                           &pd, sizeof pd, NULL, 0, NULL)) {
            kerr("nv-chan", "%s: RM would not take our page directory", name); return -1;
        }
    }

    /* 3. The GPFIFO channel itself, directly under the DEVICE.
     *
     * nouveau's r535 path (fifo.c r535_chan_alloc) allocates the channel under
     * the device with only hVASpace set - it creates NO separate RM channel
     * group (TSG) or context-share object; the runlist grouping is internal to
     * RM.  We match that: hContextShare = 0.  USERD lives in our sysmem pool so
     * the CPU can write GP_PUT with no BAR1 window; RM allocates the instance /
     * ramfc / method buffers itself when they are left zero. */
    /* A direct GSP client owns its ChID heap and therefore pins a slot.  A Linux
     * host-RM client must never do that: RM's global heap already contains the
     * NVKMS channels and selects a collision-free hardware ID itself. */
    ch->chid = rm->host_api ? 0xffffffffu : (u32)idx + 1;
    channel_alloc_params_t chp = { 0 };
    chp.gpfifo_offset = VA_GPFIFO;
    chp.gpfifo_entries = GPFIFO_ENTRIES;
    /* Match nouveau's flags EXACTLY: set USERD_INDEX_VALUE and PAGE_VALUE from
     * our chid, PAGE_FIXED=TRUE, but INDEX_FIXED=FALSE (nouveau leaves it clear;
     * forcing it makes RM refuse the channel with INVALID_STATE(64)). */
    chp.flags = NVOS04_FLAGS_CHANNEL_TYPE_PHYSICAL;
    if (!rm->host_api)
        chp.flags |= NVOS04_FLAGS_USERD_PAGE_FIXED
                   | NVOS04_USERD_INDEX_VALUE(ch->chid)
                   | NVOS04_USERD_INDEX_PAGE_VALUE(ch->chid);
    if (golden) chp.flags |= NVOS04_FLAGS_PRIVILEGED_CHANNEL;  /* GR golden init */
    chp.h_context_share = 0;
    chp.h_vaspace = h_vaspace(idx);
    chp.engine_type = engine_type;
    /* Host RM resolves this memory handle and supplies errorNotifierMem to
     * GSP itself. Do not manufacture that reserved physical descriptor. */
    chp.h_object_error = nv_codec_notifier_setup(ch, idx);
    /* Host RM consumes a real client USERD memory handle, exactly as
     * nvDmaAllocUserD()/UVM do.  Request NVIDIA-push's precise DMA tuple; the
     * nvrm bridge recognizes it and selects NV01_MEMORY_SYSTEM because BAR1 is
     * poisoned on this machine.  Direct-GSP retains its physical VRAM descriptor.
     *
     * USERD in VRAM - nouveau's r535 AND r570 both hardcode userdMem.addressSpace
     * = VIDMEM(2), cacheAttrib=1 for EVERY GSP channel (r535/fifo.c:130-131,
     * r570/fifo.c:75-76); there is no per-region aperture choice and no sysmem
     * variant (the earlier sysmem attempt was based on a comment that turned out
     * factually wrong - confirmed by the constant audit).  The host reads GP_PUT
     * from USERD via the runlist entry's physical USERD pointer, through L2, so we
     * write GP_PUT through the L2-COHERENT BAR1 window if BAR1 happens to be 1:1,
     * otherwise through the BAR0/PRAMIN window followed by an explicit L2
     * write-back+invalidate (FB_FLUSH in submit_and_wait) so the host's next read
     * misses L2 and takes our fresh value from VRAM.  USERD is CHID-INDEXED via
     * the INDEX_VALUE/PAGE_VALUE flags; whether the host samples GP_PUT at the
     * page base or at base+(chid%8)*0x200 is not provable from the open source,
     * so we hand RM the PAGE base and write GP_PUT to BOTH the page base AND this
     * chid's slot - a harmless hedge that is correct under either interpretation.
     * The live GP_GET is read from RAMFC (see RAMFC_GP_GET), not the dead 0x88. */
    if (rm->host_api) {
        mem_alloc_params_t userd_mp = { 0 };
        userd_mp.owner = rm->client;
        userd_mp.type = 6u;                   /* NVOS32_TYPE_DMA */
        userd_mp.attr = (1u << 23);           /* PAGE_SIZE_4KB; VIDMEM; uncached */
        userd_mp.flags = NVOS32_ALLOC_FLAGS_ALIGN_FORCE | 0x00010000u;
        userd_mp.size = USERD_DESC_SIZE;
        userd_mp.alignment = USERD_DESC_SIZE;
        if (!nv_rm_alloc(c, rm, RM_DEVICE, h_userdvram(idx),
                         NV01_MEMORY_LOCAL_USER, &userd_mp, sizeof userd_mp)) {
            kerr("nv-chan", "%s: host RM USERD allocation failed", name);
            return -1;
        }
        void *userd_cpu = NULL;
        u32 map_status = nvrm_host_map_memory(rm->client, RM_SUBDEVICE,
                                               h_userdvram(idx), 0,
                                               USERD_DESC_SIZE, &userd_cpu, 0);
        if (map_status != 0 || !userd_cpu) {
            kerr("nv-chan", "%s: host RM USERD mapping failed (%u)",
                 name, map_status);
            return -1;
        }
        ch->userd_sysmem = true;
        ch->userd = (volatile u32 *)userd_cpu;
        ch->userd_bar1 = NULL;
        ch->userd_page_fb = ch->userd_fb = 0;
        ch->userd[USERD_GP_GET / 4] = 0;
        ch->userd[USERD_GP_PUT / 4] = 0;
        chp.h_userd_memory[0] = h_userdvram(idx);
        chp.userd_offset[0] = 0;
        kinfo("nv-chan", "%s: official host-RM sysmem USERD handle %#x mapped at %p",
              name, h_userdvram(idx), userd_cpu);
    } else {
        u64 userd_page = 0;
        if (!nv_vram_alloc(c, rm, h_userdvram(idx), 0x1000, &userd_page)) {
            kerr("nv-chan", "%s: no VRAM for USERD", name); return -1;
        }
        ch->userd_sysmem = false;
        ch->userd = NULL;
        ch->userd_page_fb = userd_page;
        ch->userd_fb = userd_page + (u64)(ch->chid & 7u) * 0x200u;
        nv_fb_wr32(c, ch->userd_page_fb + USERD_GP_PUT, 0);
        nv_fb_wr32(c, ch->userd_page_fb + USERD_GP_GET, 0);
        nv_fb_wr32(c, ch->userd_fb + USERD_GP_PUT, 0);
        nv_fb_wr32(c, ch->userd_fb + USERD_GP_GET, 0);
        /* Keep a BAR1 view only if BAR1 is a true 1:1 framebuffer window over this
     * USERD (Resizable BAR); on a GSP card RM usually owns the BAR1 page tables
     * and the raw offset reads 0xBADF poison, in which case we fall back to
     * PRAMIN + FB_FLUSH.  (Programming our own BAR1 PTE from bar1PdeBase is the
     * next step if the RAMFC probe shows GP_PUT still isn't seen.) */
        ch->userd_bar1 = NULL;
        if (c->vram_base && c->vram_aperture &&
            ch->userd_fb + 0x200u <= (u64)c->vram_aperture) {
            volatile u32 *bar1 = (volatile u32 *)vmm_map_mmio(c->vram_base + ch->userd_fb, 0x200);
            if (bar1) {
                bar1[USERD_GP_GET / 4] = 0x00c0ffeeu;
                __asm__ volatile("sfence" ::: "memory");
                if (nv_fb_rd32(c, ch->userd_fb + USERD_GP_GET) == 0x00c0ffeeu)
                    ch->userd_bar1 = bar1;
                nv_fb_wr32(c, ch->userd_fb + USERD_GP_GET, 0);
            }
        }
        kinfo("nv-chan", "%s: USERD in VRAM page %#llx (chid %u slot %#llx); coherent BAR1 %s",
              name, (unsigned long long)ch->userd_page_fb, ch->chid,
              (unsigned long long)ch->userd_fb,
              ch->userd_bar1 ? "1:1 VERIFIED" : "not 1:1 - PRAMIN+FB_FLUSH");
        chp.userd_mem.base = ch->userd_page_fb;
        chp.userd_mem.size = USERD_DESC_SIZE;
        chp.userd_mem.address_space = ADDR_SPACE_VIDMEM;
        chp.userd_mem.cache_attrib = 1;
    }
    /* The instance block and RAMFC MUST be in VRAM on a GSP-client card - RM
     * refuses a sysmem instance block (INVALID_PARAMETER).  Allocate one VRAM
     * chunk; RAMFC sits at its base, exactly as nouveau r570 does
     * (args->ramfcMem.base = inst_addr).  The DESCRIBED sizes must be the class
     * sizes (gf100_chan_inst = 0x1000, ramfc = 0x200), not our backing size. */
    if (!rm->host_api) {
        u64 inst_fb = 0;
        if (!nv_vram_alloc(c, rm, h_instvram(idx), INSTANCE_BYTES, &inst_fb)) {
            kerr("nv-chan", "%s: no VRAM for the instance block", name); return -1;
        }
        ch->ramfc_fb = inst_fb;
        chp.instance_mem.base = inst_fb;
        chp.instance_mem.size = INST_DESC_SIZE;
        chp.instance_mem.address_space = ADDR_SPACE_VIDMEM;
        chp.instance_mem.cache_attrib = 1;
        chp.ramfc_mem.base = inst_fb;
        chp.ramfc_mem.size = RAMFC_DESC_SIZE;
        chp.ramfc_mem.address_space = ADDR_SPACE_VIDMEM;
        chp.ramfc_mem.cache_attrib = 1;
    } else {
        ch->ramfc_fb = 0;
        kinfo("nv-chan", "%s: instance/RAMFC owned by native host RM", name);
    }
    /* The method buffer's size is not ours to choose: RM computes the CE fault
     * method-buffer size and validates ours against it.  Query it (nouveau does
     * the same via NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE) instead of
     * hardcoding 0x4000. */
    u32 mthdbuf_size = 0, got_mb = 0;
    if (!rm->host_api && nv_rm_control(c, rm, RM_SUBDEVICE,
                      NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE,
                      &mthdbuf_size, sizeof mthdbuf_size,
                      &mthdbuf_size, sizeof mthdbuf_size, &got_mb) &&
        got_mb >= sizeof mthdbuf_size && mthdbuf_size > 0) {
        if (mthdbuf_size > MTHDBUF_BYTES) {
            kwarn("nv-chan", "%s: RM wants a %u-byte method buffer, larger than our %u pool slot",
                  name, mthdbuf_size, (u32)MTHDBUF_BYTES);
        }
        kinfo("nv-chan", "%s: method-buffer size from RM = %u bytes", name, mthdbuf_size);
    } else if (!rm->host_api) {
        mthdbuf_size = MTHDBUF_BYTES;      /* fall back to our slot size */
        kwarn("nv-chan", "%s: could not read the method-buffer size; using %u", name, mthdbuf_size);
    }
    if (!rm->host_api) {
        chp.mthdbuf_mem.base = pool_phys_at(ch, mthdbuf_off);
        chp.mthdbuf_mem.size = mthdbuf_size;
        chp.mthdbuf_mem.address_space = ADDR_SPACE_SYSMEM;
    }
    chp.internal_flags = golden ? INTERNALFLAGS_ADMIN : INTERNALFLAGS;  /* golden = ADMIN */

    /* Device-parented (bare) channel: GSP auto-wraps it in a TSG with the DEFAULT
     * (SYNC/VEID0) subcontext - EXACTLY what nouveau r570_chan_alloc uses
     * (GROUP_CHANNEL_THREAD=DEFAULT, fifo.c:61).  An explicit TSG + ASYNC
     * context-share was tried and is WRONG: it moved the compute RC from
     * GR_EXCEPTION(13) to PBDMA_ERROR(32) without running the grid, because
     * nouveau does not use it.  Keep the proven bare path for every engine. */
    if (!nv_rm_alloc(c, rm, RM_DEVICE, h_channel(idx), BLACKWELL_CHANNEL_GPFIFO_B,
                     &chp, sizeof chp)) {
        u32 base_status = nv_last_alloc_status;
        /* On the first channel, when we hit the INVALID_PARAMETER(59) we have
         * been chasing, sweep single-field variations so ONE boot's log tells us
         * exactly which value RM objects to (see nv_chan_diag_sweep). */
        if (idx == CH_COPY && base_status == 59) {
            u32 win_handle = 0, win_engine = 0; int win_userd_vidmem = 0;
            if (nv_chan_diag_sweep(c, rm, &chp, name,
                                   &win_handle, &win_engine, &win_userd_vidmem)) {
                ch->h_channel = win_handle;
                engine_type = win_engine;
                kinfo("nv-chan", "%s: adopted the working variation (engineType=%u, handle %08x)%s",
                      name, win_engine, win_handle,
                      win_userd_vidmem ? " - USERD is in VRAM; GP_PUT needs a BAR window "
                                         "before submission works" : "");
                goto channel_ready;
            }
            /* No swept field opened it - test the structural divergences: a page
             * directory in VRAM instead of sysmem, and the auto-wrapped TSG. */
            nv_chan_diag_vram_pd(c, rm, &chp, name);
            nv_chan_diag_tsg(c, rm, &chp, name);
        }
        kerr("nv-chan", "%s: the GPFIFO channel could not be allocated (%u)",
             name, base_status);
        return -1;
    }
    ch->h_channel = h_channel(idx);
channel_ready:;

    /* Physical channel IDs belong to host RM's global heap.  Query the ID that
     * was actually assigned; channel_alloc_params.cid is merely a software
     * sequence number and must never be used as a doorbell vector. */
    if (rm->host_api) {
        u32 channel_handle = ch->h_channel;
        u32 hardware_chid = 0xffffffffu;
        channel_list_params_t list = {
            .num_channels = 1,
            .p_channel_handle_list = (u64)(uintptr_t)&channel_handle,
            .p_channel_list = (u64)(uintptr_t)&hardware_chid,
        };
        if (!nv_rm_control(c, rm, RM_DEVICE,
                           NV0080_CTRL_CMD_FIFO_GET_CHANNELLIST,
                           &list, sizeof list, &list, sizeof list, NULL) ||
            hardware_chid == 0xffffffffu) {
            kerr("nv-chan", "%s: host RM did not return the allocated hardware ChID",
                 name);
            return -1;
        }
        ch->chid = hardware_chid;

        work_submit_token_params_t token = { 0 };
        if (!nv_rm_control(c, rm, ch->h_channel,
                           NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,
                           &token, sizeof token, &token, sizeof token, NULL) ||
            token.work_submit_token == 0) {
            kerr("nv-chan", "%s: host RM did not return a work-submit token",
                 name);
            return -1;
        }
        ch->work_submit_token = token.work_submit_token;
        ch->work_submit_token_valid = true;
        kinfo("nv-chan", "%s: host RM assigned hardware ChID %u and work-submit token %#x",
              name, ch->chid, ch->work_submit_token);
    }

    /* Two-phase (physical-then-virtual) PROMOTE_CTX was an experiment that is
     * WRONG per the authoritative RM source: NV2080_CTRL_CMD_GPU_PROMOTE_CTX is
     * ONE call whose per-buffer entry carries BOTH a physical half (bInitialize +
     * gpuPhysAddr) and a virtual half (gpuVirtAddr, gated by bNonmapped).  OGKM
     * kgrobjPromoteContext (kernel_graphics_object.c) and nouveau
     * r535_gr_promote_ctx (r535/gr.c) both emit every entry once with PA+VA
     * together (bNonmapped=1 only for PRIV_ACCESS_MAP).  A standalone VA-only
     * second call finds no already-registered buffer record and returns
     * NV_ERR_OBJECT_NOT_FOUND(87) - exactly the phase-2 refusal we observed.  The
     * single combined promote (nv_gr_promote, step 7b) was ACCEPTED on this card,
     * so route compute through the same proven path as 3D. */
    bool compute_twophase = false;

    /* 6. The engine object, under the channel.  A golden channel has none.
     *    Nouveau-style GR classes defer this to step 7b, after PROMOTE_CTX: the reference
     *    (nvkm uchan_object_new: cctx_get BEFORE oclass ctor) fetches/promotes
     *    the channel's GR context before constructing the class object.  Doing
     *    the object first put the channel live with its object bound before RM
     *    knew which context buffers back its save/restore state, and the first
     *    grid faulted with ROBUST_CHANNEL_GR_EXCEPTION(13).
     *
     *    The compute-only tinygrad experiment is deliberately different:
     *    tinygrad constructs the compute class before issuing its physical and
     *    virtual PROMOTE_CTX calls, while the channel is still unscheduled.
     *    Copy/falcon also keep their proven object-first order. */
    if (!golden && (!gr_class || compute_twophase)) {
        if (!nv_rm_alloc(c, rm, h_channel(idx), h_object(idx), obj_class,
                         (void *)obj_params, obj_params_size)) {
            kerr("nv-chan", "%s: the %#x engine object could not be allocated",
                 name, obj_class); return -1;
        }
    }

    /* Complete both compute promotions before BIND/SCHEDULE, matching
     * tinygrad's external-client ordering.  The previous build scheduled the
     * channel and promoted before constructing CEC0; phase 1 was accepted but
     * phase 2 was refused with RM status 87. */
    if (compute_twophase) {
        if (g_golden_ready) {
            kinfo("nv-chan", "%s: class allocated; promoting MAIN+PM+PATCH before schedule", name);
            if (!nv_gr_promote_compute_twophase(c, rm, &ch->vmm, h_channel(idx),
                                                 0x00460000u + (u32)idx * 0x20u))
                kwarn("nv-chan", "%s: two-phase GR PROMOTE_CTX failed - dispatch will fault", name);
        } else {
            kwarn("nv-chan", "%s: GR golden context not ready - this channel cannot dispatch", name);
        }
    }

    /* 7. Bind the channel to the engine, then schedule (enable) it.
     *
     * The GOLDEN channel is deliberately NEITHER bound NOR scheduled: OGKM
     * (kgraphicsCreateGoldenImageChannel, "we only need this channel created,
     * will not submit any work") and nouveau (r535_gr_oneinit) both alloc the
     * golden channel + 3D object (which triggers golden-image generation in RM)
     * and free it WITHOUT ever binding or scheduling it.  Scheduling it lets GSP
     * switch its still-uninitialized GR context onto the engine and cache THAT as
     * the golden image - a deterministically empty SM topology, so every real
     * channel then promotes a context with no TPCs for CWD to dispatch to and the
     * grid hangs with no exception (CWD_FS=0).  So: schedule only real channels.
     *
     * ORDERING: GR promotion must precede the GR object and scheduling.  For an
     * externally-owned Falcon VAS, Linux Nouveau binds/schedules the channel,
     * then maps its vctx and sends the VA+size promotion. */

    /* 7b. The remaining Nouveau-style GR channel (3D): PROMOTE the golden
     *     context into the channel, THEN allocate the class object.  The
     *     reference constructs the class object only after the channel context is
     *     fetched/promoted (nvkm cctx_get precedes oclass ctor), and it matches
     *     nv_gr_bring_up_golden, which promotes before allocating its 3D class.
     *     The previous order (object at step 6, promote here) faulted the first
     *     grid with GR_EXCEPTION(13), exactly the signal the old comment predicted.
     *     Skipped cleanly if golden isn't ready. */
    if (!golden && gr_class && !compute_twophase) {
        if (rm->host_api) {
            /* In the native 595 stack CPU-RM is the GSP client: constructing
             * CE97/CEC0 invokes KernelGraphicsObject context allocation and
             * physical promotion.  The external-VAS virtual mapping/promotion
             * is completed below from RM's retained resource descriptions. */
            kinfo("nv-chan", "%s: native host RM allocated the GR context; binding its external-VAS resources", name);
        } else if (g_golden_ready) {
            kinfo("nv-chan", "%s: promoting the GR context into this channel (before object alloc)", name);
            bool promoted = nv_gr_promote(c, rm, false, &ch->vmm, h_channel(idx),
                                          0x00460000u + (u32)idx * 0x20u);
            if (!promoted)
                kwarn("nv-chan", "%s: per-channel GR PROMOTE_CTX failed - draws will fault", name);
        } else {
            kwarn("nv-chan", "%s: GR golden context not ready - this channel cannot draw yet", name);
        }
        /* Now the GR class object, after the context is in place. */
        if (!nv_rm_alloc(c, rm, h_channel(idx), h_object(idx), obj_class,
                         (void *)obj_params, obj_params_size)) {
            kerr("nv-chan", "%s: the %#x engine object could not be allocated "
                 "(post-promote)", name, obj_class); return -1;
        }
    }

    /* Host RM has now allocated and physically initialized the private engine
     * resources.  For an external VAS, Linux/UVM maps and virtually promotes
     * them before it enables a GR channel.  Falcon scheduling does not have
     * GR's hard bIsContextBound guard, but binding it here is equally required
     * before its first method stream and avoids ever running an unbound engine. */
    if (!golden && rm->host_api &&
        (gr_class || obj_class == NVCFB0_VIDEO_DECODER ||
                     obj_class == NVCFB7_VIDEO_ENCODER)) {
        if (!host_bind_channel_resources(c, rm, ch, idx, engine_type, gr_class)) {
            kerr("nv-chan", "%s: native RM resources were not bound; refusing "
                  "to schedule an incomplete channel", name);
            return -1;
        }
    }

    /* Bind the channel to the engine, then schedule (enable) it.  Every context
     * required by a real channel has been registered and mapped above. */
    if (!golden) {
        bind_params_t bind = { .engine_type = engine_type };
        if (!nv_rm_control(c, rm, h_channel(idx), NVA06F_CTRL_CMD_BIND, &bind, sizeof bind, NULL, 0, NULL))
            kwarn("nv-chan", "%s: channel BIND was refused (some parts do it implicitly)", name);

        schedule_params_t sched = {
            .enable = 1, .skip_submit = 0, .skip_enable = 0
        };
        if (!nv_rm_control(c, rm, h_channel(idx), NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                           &sched, sizeof sched, NULL, 0, NULL)) {
            kerr("nv-chan", "%s: the channel would not schedule (enable)", name); return -1;
        }
    }

    /* Linux Nouveau's direct-GSP order is channel alloc -> BIND -> SCHEDULE ->
     * Falcon vctx map/PROMOTE.  The earlier pre-schedule placement copied an
     * internal-RM retained-resource sequence instead of the API we implement. */
    if (!golden && (obj_class == NVCFB0_VIDEO_DECODER || obj_class == NVCFB7_VIDEO_ENCODER)) {
        if (rm->host_api) {
            kinfo("nv-chan", "%s: native host RM Falcon context is fully bound", name);
        } else {
            kinfo("nv-chan", "%s: promoting the Falcon external-VAS context after BIND/SCHEDULE", name);
            if (!nv_flcn_promote(c, rm, ch, engine_type, h_channel(idx),
                                 0x004A0000u + (u32)idx * 0x20u)) {
            schedule_params_t stop = {
                .enable = 0, .skip_submit = 0, .skip_enable = 0
            };
            (void)nv_rm_control(c, rm, h_channel(idx),
                                NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                                &stop, sizeof stop, NULL, 0, NULL);
            kerr("nv-chan", "%s: Falcon context promotion failed; channel disabled", name);
            return -1;
            }
        }
    }

    /* Learn this engine's REAL runlist id from the device-info table so the
     * doorbell can carry it.  Without this we were guessing (sweeping 0..7),
     * which cannot reach COPY4's actual runlist (typically >= 8). */
    ch->runlist_known = nv_query_runlist(c, rm, engine_type, &ch->runlist_id);

    ch->open = true;
    kinfo("nv-chan", "%s channel OPEN and scheduled (object %#x on engine %#x, "
          "runlist %d)", name, obj_class, engine_type,
          ch->runlist_known ? (int)ch->runlist_id : -1);
    return 0;
}

/* Capture the GR golden context once, before any 3D/compute channel is opened.
 * On success g_golden_ready is set and per-channel promotes can run; on any
 * failure it logs why and leaves g_golden_ready false - the proven copy channel
 * is never touched either way. */
#define GOLDEN_3D_HANDLE 0x9d000000u
static void nv_gr_bring_up_golden(nv_card_t *c, nv_rm_t *rm) {
    if (g_golden_ready) return;
    kinfo("nv-chan", "GR golden context: bringing up once (prerequisite for 3D + compute)");

    /* Reliable topology probe (GSP-routed, so it bypasses the PLM lock that makes
     * a raw CWD_FS/PGRAPH CPU read return a meaningless 0).  If GSP reports
     * gpcMask==0 or numSm==0 the GR engine itself has no compute resources and no
     * client fix helps; nonzero proves the topology is real and the "no SM"
     * symptom is downstream.  GET_FERMI_GPC_INFO=0x20800137 {u32 gpcMask};
     * INTERNAL_STATIC_KGR_GET_GLOBAL_SM_ORDER=0x20800a22.  Its ABI is eight
     * engine records of 240 twelve-byte SM IDs followed by u16 numSm/numTpc;
     * the old 1024-byte probe was guaranteed to be rejected. */
    {
        struct { u32 gpcMask; } gi = { 0 }; u32 gl = 0;
        if (nv_rm_control_prefer_internal(c, rm, 0x20800137u, &gi, sizeof gi,
                                          &gi, sizeof gi, &gl))
            kinfo("nv-chan", "topology: FERMI_GPC_INFO gpcMask=%#x", gi.gpcMask);
        else
            kwarn("nv-chan", "topology: FERMI_GPC_INFO query refused");
        typedef struct {
            struct { u16 gpc, local_tpc, local_sm, global_tpc, virtual_gpc,
                         migratable_tpc; } sm[240];
            u16 num_sm, num_tpc;
        } sm_order_engine_t;
        typedef struct { sm_order_engine_t engine[8]; } sm_order_params_t;
        _Static_assert(sizeof(sm_order_params_t) == 23072,
                       "R570 static global-SM-order ABI size");
        static sm_order_params_t smo; u32 sl = 0;
        memset(&smo, 0, sizeof smo);
        if (nv_rm_control_prefer_internal(c, rm, 0x20800a22u, &smo, sizeof smo,
                                          &smo, sizeof smo, &sl) && sl >= sizeof smo) {
            u32 numSm = smo.engine[0].num_sm, numTpc = smo.engine[0].num_tpc;
            kinfo("nv-chan", "topology: GLOBAL_SM_ORDER numSm=%u numTpc=%u (reply %u B)",
                  numSm, numTpc, sl);
        } else {
            kwarn("nv-chan", "topology: GLOBAL_SM_ORDER query refused (reply %u B)", sl);
        }
    }

    /* 1. A throwaway GR channel with no engine object. */
    if (open_engine_channel(c, rm, CH_GOLDEN, NV2080_ENGINE_TYPE_GRAPHICS,
                            0, NULL, 0, "GR-golden", true) != 0) {
        kwarn("nv-chan", "GR golden: the golden channel would not open; 3D/compute unavailable");
        return;
    }
    /* 2. Describe the GR context buffers from the device. */
    if (!nv_gr_query_ctxbufs(c, rm)) return;
    /* 3. Promote the golden context (all buffers fresh; globals kept for reuse). */
    if (!nv_gr_promote(c, rm, true, &channels[CH_GOLDEN].vmm, h_channel(CH_GOLDEN), 0x00470000u)) {
        kwarn("nv-chan", "GR golden: PROMOTE_CTX(golden) refused; 3D/compute unavailable");
        return;
    }
    /* 4. Allocate the 3D class on the golden channel: this is what makes RM run
     *    the context-init microcode and cache the golden image. */
    if (!nv_rm_alloc(c, rm, h_channel(CH_GOLDEN), GOLDEN_3D_HANDLE, BLACKWELL_B, NULL, 0)) {
        kwarn("nv-chan", "GR golden: 3D-class alloc on the golden channel refused (%u)",
              nv_last_alloc_status);
        return;
    }
    /* 5. RM has cached the image; drop the throwaway 3D object + channel + vaspace.
     *    The GLOBAL context buffers stay allocated (real channels reference them);
     *    the golden channel's own sysmem pool / VRAM tables leak by design (a few
     *    MiB, once per boot) rather than risk freeing something still referenced. */
    (void)nv_rm_free(c, rm, h_channel(CH_GOLDEN), GOLDEN_3D_HANDLE);
    (void)nv_rm_free(c, rm, RM_DEVICE, h_channel(CH_GOLDEN));
    (void)nv_rm_free(c, rm, RM_DEVICE, h_vaspace(CH_GOLDEN));
    /* Nouveau destroys the temporary golden vctx here.  Reclaim its local
     * MAIN/PATCH allocations too; keeping them leaked consumed 3.3 MiB of the
     * narrow contiguous range needed by the display surface.  Global buffers
     * remain alive because every real GR context references them. */
    for (int i = 0; i < g_ctxbuf_nr; i++) {
        if (!g_ctxbuf[i].global)
            (void)nv_rm_free(c, rm, RM_DEVICE, 0x00470000u + (u32)i);
    }
    g_golden_ready = true;
    kinfo("nv-chan", "GR golden context READY - 3D and compute channels can now be promoted");
}

int nv_chan_open(nv_card_t *c, nv_rm_t *rm) {
    kinfo("nv-chan", "GPU diagnostic revision external-pdb-fault-v1: explicit root TLB flush, bounded channel/MMU snapshots");
    if (channels[CH_COPY].open) return 0;

    /* Learn which engines the card actually exposes before binding to one. */
    nv_query_engines(c, rm);

    /* The 2D/copy channel first - it is the one the self-test proves end to
     * end, and the simplest engine to bring up. */
    u32 copy_engine = nv_pick_async_copy_engine(c);
    copy_params_t cp = { .version = 1, .engine_type = copy_engine };
    if (open_engine_channel(c, rm, CH_COPY, copy_engine,
                            BLACKWELL_DMA_COPY_B, &cp, sizeof cp, "copy/2D", false) != 0)
        return -1;

    kinfo("nv-chan", "the copy channel is open - the card can be given work over "
                     "the ring; bringing up the other engines now");
    return 0;
}

/* Bring up the remaining engines: 3D, compute, video decode and encode.  Each
 * is a separate channel following the same recipe; every one logs whether it
 * opened or exactly which alloc the card refused, so a boot maps the whole
 * render/codec pipeline in one pass.  Returns how many opened. */
int nv_chan_open_engines(nv_card_t *c, nv_rm_t *rm) {
    int opened = 0;

    /* GR golden context first - the prerequisite that lets 3D and compute
     * actually draw/dispatch (the copy engine needed none of this).
     *
     * ORDER IS A RESOURCE REQUIREMENT, not cosmetic.  NVIDIA 595 creates its
     * golden-image channel from the FIFO post-scheduling-enable callback
     * (_kgraphicsPostSchedulingEnableHandler ->
     * kgraphicsCreateGoldenImageChannel), before ordinary client surfaces.
     * The on-card log agrees: with that order golden CE97 and all four real
     * engine channels opened and two real sm_120 grids completed.  Moving the
     * 23.6 MiB display allocation ahead of this call shifted the 50.3 MiB
     * ATTRIBUTE_CB inside RM's constrained contiguous region and made the
     * golden CE97 constructor return NV_ERR_NO_MEMORY (81); later channels then
     * ran into the same exhausted/fragmented client heap. */
    if (rm->host_api)
        kinfo("nv-chan", "native host RM owns the one-time GR golden image and client context lifecycle");
    else
        nv_gr_bring_up_golden(c, rm);

    /* 3D / graphics.  BLACKWELL_B (0xce97) takes no allocation parameters. */
    if (open_engine_channel(c, rm, CH_GFX, NV2080_ENGINE_TYPE_GRAPHICS,
                            BLACKWELL_B, NULL, 0, "3D/graphics", false) == 0) {
        opened++;
        /* Compute shares the graphics engine; add a compute object on the same
         * channel so a shader can be launched without a second channel. */
        if (nv_rm_alloc(c, rm, channels[CH_GFX].h_channel, h_object2(CH_GFX),
                        BLACKWELL_COMPUTE_B, NULL, 0)) {
            kinfo("nv-chan", "compute object 0xcec0 allocated on the 3D channel");
            opened++; /* one GR channel now provides both logical capabilities */
        } else
            kwarn("nv-chan", "compute object 0xcec0 was refused on the 3D channel");
    }

    /* One GR channel can host CE97 and CEC0 on separate subchannels.  The 05:38
     * boot proved both dedicated GR contexts work, but their duplicate 0x349000
     * MAIN buffers leave only ~12 MiB contiguous below the next RM reservation,
     * less than a 2560x1440 XRGB scanout (14,745,600 B).  Use the already-open
     * CEC0 object on CH_GFX for compute/runtime 3D and reserve scanout now. */
    /* Native NVKMS owns two real scanout allocations per active head.  The
     * legacy direct-GSP path still needs its private preallocation, but doing
     * that under host RM wastes tens of MiB and can starve GR/codec contexts. */
    if (!rm->host_api)
        nv_disp_prealloc_scanout(c, rm);

    /* Video decode (NVDEC) and encode (NVENC): the objects take a small
     * allocation-parameters struct naming the engine instance.  Bind to the
     * NVDEC/NVENC engine indices the card actually reports. */
    u32 nvdec_engine = nv_pick_engine(NV2080_ENGINE_TYPE_NVDEC0);
    u32 nvenc_engine = nv_pick_engine(NV2080_ENGINE_TYPE_NVENC0);
    video_obj_params_t nvdec_vp = { .size = sizeof(video_obj_params_t),
                                    .prohibit_multiple = 0,
                                    .engine_instance =
                                        nvdec_engine >= NV2080_ENGINE_TYPE_NVDEC0 ?
                                        nvdec_engine - NV2080_ENGINE_TYPE_NVDEC0 : 0u };
    /* NVENC engine IDs are 27,28,29,63 for instances 0..3.  Do not subtract
     * blindly across the NVDEC hole: Blackwell reports NVENC1 as engine 28. */
    u32 nvenc_instance = nvenc_engine == 27u ? 0u :
                         nvenc_engine == 28u ? 1u :
                         nvenc_engine == 29u ? 2u :
                         nvenc_engine == 63u ? 3u : 0u;
    video_obj_params_t nvenc_vp = { .size = sizeof(video_obj_params_t),
                                    .prohibit_multiple = 0,
                                    .engine_instance = nvenc_instance };
    kinfo("nv-chan", "video object routing: NVDEC engine %#x instance %u; NVENC engine %#x instance %u",
          nvdec_engine, nvdec_vp.engine_instance,
          nvenc_engine, nvenc_vp.engine_instance);
    if (open_engine_channel(c, rm, CH_NVDEC, nvdec_engine,
                            NVCFB0_VIDEO_DECODER, &nvdec_vp, sizeof nvdec_vp,
                            "NVDEC/decode", false) == 0)
        opened++;
    if (open_engine_channel(c, rm, CH_NVENC, nvenc_engine,
                            NVCFB7_VIDEO_ENCODER, &nvenc_vp, sizeof nvenc_vp,
                            "NVENC/encode", false) == 0)
        opened++;

    kinfo("nv-chan", "engine bring-up finished: %d of 4 further engines opened "
                     "(3D+compute share one GR channel, NVDEC, NVENC)", opened);
    if (rm->host_api) {
        u32 engines = 0;
        if (channels[CH_GFX].open) engines |= 1u << NV_ENGINE_3D;
        if (channels[CH_COPY].open) engines |= 1u << NV_ENGINE_COPY;
        if (channels[CH_NVENC].open) engines |= 1u << NV_ENGINE_ENCODE;
        if (channels[CH_NVDEC].open) engines |= 1u << NV_ENGINE_DECODE;
        nv_telemetry_bind_host(c, rm->client, RM_DEVICE, RM_SUBDEVICE, engines);
    }
    return opened;
}

bool nv_chan_is_open(void) { return channels[CH_COPY].open; }

/* ------------------------------------------------------ submitting work */

/* Copy-engine (clc7b5, inherited by cab5) method offsets. */
#define CE_OFFSET_IN_UPPER   0x400
#define CE_OFFSET_IN_LOWER   0x404
#define CE_OFFSET_OUT_UPPER  0x408
#define CE_OFFSET_OUT_LOWER  0x40C
#define CE_PITCH_IN          0x410
#define CE_PITCH_OUT         0x414
#define CE_LINE_LENGTH_IN    0x418
#define CE_LINE_COUNT        0x41C
#define CE_SET_SEMAPHORE_A   0x240
#define CE_SET_SEMAPHORE_B   0x244
#define CE_SET_SEMAPHORE_PAYLOAD 0x248
#define CE_LAUNCH_DMA        0x300
#define CE_SET_REMAP_CONST_B 0x704
#define CE_SET_REMAP_COMPONENTS 0x708
/* C8B5/CAB5: one four-byte destination component, selected from CONST_B.
 * Matches uvm_hal_hopper_ce_memset_4(), inherited by Blackwell. */
#define CE_REMAP_FILL32      (5u | (3u << 16))
#define CE_REMAP_ENABLE      (1u << 10)
/* LAUNCH_DMA flags: NON_PIPELINED(2 in 1:0) | FLUSH_ENABLE_TRUE(2:2) |
 * SEMAPHORE_TYPE RELEASE_ONE_WORD (1 in 4:3) | SRC_MEMORY_LAYOUT PITCH(1 in 7:7)
 * | DST_MEMORY_LAYOUT PITCH(1 in 8:8) | MULTI_LINE_ENABLE_TRUE(1 in 9:9) |
 * DISABLE_PLC_TRUE (1 in 26:26).
 * The two layout bits are REQUIRED: left
 * zero they default to BLOCKLINEAR, so the copy engine reads our linear buffers
 * as tiled surfaces, faults, and halts the channel before the semaphore fires -
 * NVIDIA's own CE code sets both to PITCH on every linear copy (cla0b5.h 7:7/8:8,
 * channel_utils.c).  MULTI_LINE_ENABLE is equally required whenever LINE_COUNT
 * is greater than one.  Without bit 9 the returned hardware copied only row 0:
 * the runtime self-test's fill/move row-0 samples passed, while image row 3
 * retained the old red scanout pixel.
 *
 * CAB5 inherits C8B5's PLC control.  NVIDIA's official Blackwell UVM HAL
 * inherits uvm_hal_ampere_ce_plc_mode_c7b5() and therefore adds
 * DISABLE_PLC_TRUE to every ordinary copy and semaphore release.  Omitting it
 * survived the large, aligned proof frames but the first desktop-sized 32x32
 * damage upload (fence 0x5052...) stopped the CE channel.  Keep the production
 * launch word identical to that official HAL requirement. */
#define CE_LAUNCH   (0x2u | (1u << 2) | (1u << 3) | (1u << 7) | (1u << 8) | \
                     (1u << 9) | (1u << 26))

#define SUBCH_COPY  0

/* Write a method header (auto-increment) + advance the pushbuffer cursor. */
static void pb_method(nv_channel_t *ch, u32 subch, u32 method, u32 count) {
    u32 hdr = (1u << 29) | ((count & 0x1FFFu) << 16) |
              ((subch & 0x7u) << 13) | ((method >> 2) & 0xFFFu);
    *(volatile u32 *)(ch->pushbuf + ch->pb_at) = hdr; ch->pb_at += 4;
}
static void pb_data(nv_channel_t *ch, u32 v) {
    *(volatile u32 *)(ch->pushbuf + ch->pb_at) = v; ch->pb_at += 4;
}

/* Flush (write back + invalidate) a range of cache lines so the card, reading
 * our sysmem structures over PCIe, sees the bytes we just wrote rather than a
 * stale copy still sitting in the CPU's write-back cache - and, read the other
 * way, so we see what the card wrote rather than our own cached copy. */
static inline void cache_flush(const volatile void *p, u32 bytes) {
    const u8 *a = (const u8 *)p;
    u64 first = (u64)(uintptr_t)a & ~63ull;
    u64 last  = ((u64)(uintptr_t)a + bytes + 63) & ~63ull;
    for (u64 line = first; line < last; line += 64)
        __asm__ volatile("clflush (%0)" :: "r"((const void *)(uintptr_t)line) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

/* Runtime waits may sleep only inside the pinned graphics transaction, with
 * interrupts enabled and the native RM interrupt route established. Early boot,
 * legacy RM and atomic callers retain their bounded polling behavior. */
static bool nv_completion_wait_allowed(nv_channel_t *ch) {
    return ch->rm->host_api && g_render_pin && g_render_pin == proc_current() &&
           nvrm_completion_irq_ready() && sched_device_wait_allowed();
}

static u32 nv_completion_awaken(nv_channel_t *ch, u32 flag) {
    return nv_completion_wait_allowed(ch) ? flag : 0u;
}

/* Published class controls: CAB5 LAUNCH_DMA interrupt type NON_BLOCKING (6:5),
 * CEC0 REPORT_SEMAPHORE_EXECUTE awaken (2:2), and video SEMAPHORE_D awaken
 * (8:8, C9B7/C5B0). Do not request a blocking copy interrupt: the semaphore
 * remains the retirement authority, and RM owns acknowledgement of the IRQ. */
#define CE_COMPLETION_AWAKEN       (2u << 5)
#define COMPUTE_COMPLETION_AWAKEN  (1u << 2)
#define VIDEO_COMPLETION_AWAKEN    (1u << 8)

typedef struct { nv_channel_t *channel; u32 cookie; } nv_completion_wait_t;
static bool nv_completion_probe(void *context) {
    const nv_completion_wait_t *wait = context;
    cache_flush((const void *)&wait->channel->sem[0], 4);
    /* Wake on failure as well: an RC'd engine may never write the cookie.
     * This is only a wake predicate; submit_and_wait decides the outcome. */
    return wait->channel->sem[0] == wait->cookie ||
           nv_error_notification_failed(wait->channel->error_notifier);
}

/* Move bytes between a CPU buffer and an RM-owned VRAM allocation.  When the
 * Linux host RM backend owns the card, BAR0/PRAMIN writes bypass the GPU's L2
 * view and FB_FLUSH_GPU_CACHE_IRQL is intentionally not exported to clients.
 * NVIDIA's MemUtils transfer is the coherent, supported path in that mode.
 * The direct-GSP backend still owns cache maintenance and may use PRAMIN. */
static bool nv_vram_object_write(nv_channel_t *ch, u32 memory, u64 fb,
                                 u64 offset, const void *src, u64 bytes) {
    if (!bytes) return true;
    if (!src || ((offset | bytes) & 3u)) {
        kerr("nv-chan", "unaligned VRAM object write: off=%#llx bytes=%#llx",
             (unsigned long long)offset, (unsigned long long)bytes);
        return false;
    }
    if (ch->rm->host_api) {
        u32 status = nvrm_transfer_rm_memory(ch->rm->client, memory, offset,
                                              (void *)src, bytes, false);
        if (status != 0) {
            kerr("nv-chan", "coherent host-RM VRAM write %#x+%#llx failed (%u)",
                 memory, (unsigned long long)offset, status);
            return false;
        }
        return true;
    }
    const u32 *words = (const u32 *)src;
    for (u64 o = 0; o < bytes; o += 4)
        nv_fb_wr32(ch->card, fb + offset + o, words[o / 4]);
    u32 flags = 0x3u;
    return nv_rm_control(ch->card, ch->rm, RM_SUBDEVICE,
                         NV2080_CTRL_CMD_FB_FLUSH_GPU_CACHE_IRQL,
                         &flags, sizeof flags, NULL, 0, NULL);
}

static bool nv_vram_object_read(nv_channel_t *ch, u32 memory, u64 fb,
                                u64 offset, void *dst, u64 bytes) {
    if (!bytes) return true;
    if (!dst || ((offset | bytes) & 3u)) {
        kerr("nv-chan", "unaligned VRAM object read: off=%#llx bytes=%#llx",
             (unsigned long long)offset, (unsigned long long)bytes);
        return false;
    }
    if (ch->rm->host_api) {
        u32 status = nvrm_transfer_rm_memory(ch->rm->client, memory, offset,
                                              dst, bytes, true);
        if (status != 0) {
            kerr("nv-chan", "coherent host-RM VRAM read %#x+%#llx failed (%u)",
                 memory, (unsigned long long)offset, status);
            return false;
        }
        return true;
    }
    u32 flags = 0x3u;
    if (!nv_rm_control(ch->card, ch->rm, RM_SUBDEVICE,
                       NV2080_CTRL_CMD_FB_FLUSH_GPU_CACHE_IRQL,
                       &flags, sizeof flags, NULL, 0, NULL))
        return false;
    u32 *words = (u32 *)dst;
    for (u64 o = 0; o < bytes; o += 4)
        words[o / 4] = nv_fb_rd32(ch->card, fb + offset + o);
    return true;
}

/* Write GP_PUT into USERD.  USERD is VRAM; we write BOTH the page base and this
 * chid's slot (the hedge - the host reads GP_PUT at one of them and we can't
 * prove which from source).  Prefer the 1:1 BAR1 view (L2-coherent) if present,
 * else the BAR0/PRAMIN window (needs the FB_FLUSH the caller issues). */
static void userd_write_gpput(nv_channel_t *ch, u32 v) {
    if (ch->userd_sysmem) {          /* legacy path, not used now USERD is VRAM */
        ch->userd[USERD_GP_PUT / 4] = v;
        cache_flush((const void *)&ch->userd[USERD_GP_PUT / 4], 4);
        return;
    }
    if (ch->userd_bar1) {
        ch->userd_bar1[USERD_GP_PUT / 4] = v;
        __asm__ volatile("sfence" ::: "memory");
    }
    /* Always poke both slots through PRAMIN too (cheap, and the BAR1 view only
     * covers the chid slot). */
    nv_fb_wr32(ch->card, ch->userd_page_fb + USERD_GP_PUT, v);
    nv_fb_wr32(ch->card, ch->userd_fb      + USERD_GP_PUT, v);
}
/* Read the LIVE GP_GET.  On Blackwell it is in RAMFC (ramfc_fb + RAMFC_GP_GET),
 * NOT the retired USERD 0x88 - reading 0x88 always returns our init value and
 * tells us nothing about fetch progress. */
static u32 userd_read_gpget(nv_channel_t *ch) {
    if (ch->rm->host_api) return 0xffffffffu; /* host RM owns the RAMFC mapping */
    return nv_fb_rd32(ch->card, ch->ramfc_fb + RAMFC_GP_GET);
}
static u32 userd_read_gpput(nv_channel_t *ch) {
    if (ch->userd_sysmem) return ch->userd[USERD_GP_PUT / 4];
    if (ch->userd_bar1) return ch->userd_bar1[USERD_GP_PUT / 4];
    return nv_fb_rd32(ch->card, ch->userd_fb + USERD_GP_PUT);
}

/* Push one GP entry for the methods written since `start_off`, ring the
 * doorbell, and wait for the copy engine's semaphore to reach `sem_value`. */
#include "nv_channel_fault.h"

static bool submit_and_wait(nv_channel_t *ch, u32 start_off, u32 sem_value) {
    if (ch->submit_failed) return false;
    /* Latched before publication. Only an observed completion may clear it. */
    ch->submit_failed = true;
    u32 bytes = ch->pb_at - start_off;
    /* Only codec channels currently own this error slot. Refuse a channel
     * already marked by RM before publishing another ring entry. */
    if (nv_error_notification_failed(ch->error_notifier)) goto channel_error;
    u64 pb_va = VA_PUSHBUF + start_off;

    u32 e0 = (u32)(pb_va & 0xFFFFFFFCu);
    u32 e1 = (u32)((pb_va >> 32) & 0xFFu) | (((bytes >> 2) & 0x1FFFFFu) << 10);
    ch->gpfifo[ch->gp_put * 2 + 0] = e0;
    ch->gpfifo[ch->gp_put * 2 + 1] = e1;
    ch->gp_put = (ch->gp_put + 1) % GPFIFO_ENTRIES;

    /* The ring and pushbuffer are sysmem the card reads over PCIe; flush them out
     * of the CPU cache so the card sees fresh bytes. */
    cache_flush(ch->pushbuf + start_off, bytes);
    cache_flush((const void *)&ch->gpfifo[ch->gp_put ? (ch->gp_put - 1) * 2
                                                     : (GPFIFO_ENTRIES - 1) * 2], 8);
    cache_flush((const void *)&ch->sem[0], 4);       /* so our later read is fresh */

    /* Follow NVIDIA's Blackwell-selected UVM submission sequence exactly:
     * finish push/ring writes, complete a dummy BAR1 read when GP_PUT is in
     * sysmem, publish GP_PUT, apply a write barrier, then ring the doorbell. */
    if (ch->userd_sysmem && ch->submit_bar1_flush) {
        u32 ordered = *ch->submit_bar1_flush;
        __asm__ volatile("" :: "r"(ordered) : "memory");
    }
    userd_write_gpput(ch, ch->gp_put);
    __asm__ volatile("mfence" ::: "memory");

    /* The VRAM USERD write above (PRAMIN) bypasses L2, and the host reads GP_PUT
     * through L2 - so write-back + invalidate L2 or the host keeps seeing a stale
     * GP_PUT and never fetches. */
    if (!ch->userd_sysmem) {
        u32 flush_flags = 0x3u;  /* WRITE_BACK_YES(0) | INVALIDATE_YES(1) */
        (void)nv_rm_control(ch->card, ch->rm, RM_SUBDEVICE,
                            NV2080_CTRL_CMD_FB_FLUSH_GPU_CACHE_IRQL,
                            &flush_flags, sizeof flush_flags, NULL, 0, NULL);
    }

    #define NV_DOORBELL_RUNLIST_ENABLE (1u << 30)

    work_submit_token_params_t tok = {
        .work_submit_token = ch->work_submit_token_valid
                           ? ch->work_submit_token : 0
    };
    if (!ch->work_submit_token_valid) {
        u32 tok_len = 0;
        if (nv_rm_control(ch->card, ch->rm, ch->h_channel,
                          NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,
                          &tok, sizeof tok, &tok, sizeof tok, &tok_len) &&
            tok.work_submit_token != 0) {
            ch->work_submit_token = tok.work_submit_token;
            ch->work_submit_token_valid = true;
        }
    }
    /* USERD GP_PUT (what we wrote, VRAM) and the REAL GP_GET read from RAMFC.
     * Also read the RAMFC GP_BASE the host will fetch from - if GSP-RM stored our
     * ring VA there, GP_BASE == VA_GPFIFO; if not, RM never accepted our ring
     * pointer (a problem upstream of fetch). This split makes the boot conclusive:
     *   GP_BASE != VA_GPFIFO      -> RM/alloc problem (ring pointer not stored)
     *   GP_BASE ok, GP_GET stays 0 -> host still not fetching (doorbell/GP_PUT)
     *   GP_GET advances, no sem    -> host fetched; pushbuffer/copy faulted */
    /* Runtime presentation can submit hundreds of tiny copies per frame.  Keep
     * enough successful samples to prove USERD/token progress without evicting
     * the much more valuable engine-open failures from the persistent log. */
    /* Ring indices wrap, so testing GP_PUT printed another burst every lap.
     * Bound healthy traffic by submission count; all timeout/fault logs remain. */
    static u32 submission_trace_count;
    u32 trace_serial = ++submission_trace_count;
    bool trace_success = trace_serial <= 16u || (trace_serial & 0xfffu) == 0u;
    if (ch->rm->host_api) {
        if (trace_success)
            kinfo("nv-chan", "USERD(host-sysmem) GP_PUT=%u; host-owned RAMFC; chid=%u token=%#x",
                  userd_read_gpput(ch), ch->chid, tok.work_submit_token);
    } else {
        u32 rgp_lo = nv_fb_rd32(ch->card, ch->ramfc_fb + RAMFC_GP_BASE);
        u32 rgp_hi = nv_fb_rd32(ch->card, ch->ramfc_fb + RAMFC_GP_BASE_HI);
        kinfo("nv-chan", "USERD(%s) GP_PUT=%u; RAMFC GP_BASE=%#x_%08x (expect %#llx) GP_GET=%u; "
                         "chid=%u token=%#x runlist=%d",
              ch->userd_bar1 ? "bar1" : "pramin",
              userd_read_gpput(ch), rgp_hi, rgp_lo, (unsigned long long)VA_GPFIFO,
              userd_read_gpget(ch), ch->chid, tok.work_submit_token,
              ch->runlist_known ? (int)ch->runlist_id : -1);
    }

    /* DECISIVE PROBE: is the VFN doorbell region even reachable by a CPU BAR0
     * access, or is it PLM-locked under GSP so our doorbell rings are silently
     * dropped?  A read of 0xBADFxxxx means the register block is masked off from
     * us (writes go nowhere → host never notified → GP_GET stuck at 0, exactly
     * what we see).  A sane value means the doorbell reaches and the wall is the
     * runlist/channel not being live.  Offsets: VFN base 0xB80000, user region
     * +0x30000, doorbell +0x30090, priv doorbell +0x2200. */
    if (!ch->rm->host_api)
        kinfo("nv-chan", "VFN probe: doorbell(0xbb0090)=%#x vfn(0xb80000)=%#x "
                         "user(0xbb0000)=%#x privdb(0xb82200)=%#x  (0xbadf... = PLM-blocked)",
              nv_rd32(ch->card, NV_USERMODE_DOORBELL),
              nv_rd32(ch->card, 0x00b80000u),
              nv_rd32(ch->card, 0x00bb0000u),
              nv_rd32(ch->card, 0x00b82200u));

    /* Doorbell = RUNLIST_DOORBELL_ENABLE(30) | (runlistId<<16) | chid, written
     * UNMODIFIED (this is exactly nouveau's gb202_chan_doorbell_handle).  The
     * PRIMARY candidate is the REAL runlist id from the device-info table - this
     * is the fix: the old code guessed by sweeping 0..7, which cannot reach
     * COPY4's actual runlist (>= 8).  Keep the RM token and a WIDER sweep only as
     * fallbacks so a single boot still probes broadly if the table lookup missed. */
    u32 cand[80]; u32 ncand = 0;
    if (ch->rm->host_api) {
        /* Host RM's token is the complete payload.  Do not OR bits into it. */
        if (ch->work_submit_token_valid)
            cand[ncand++] = ch->work_submit_token;
    } else {
        if (ch->runlist_known)
            cand[ncand++] = NV_DOORBELL_RUNLIST_ENABLE |
                            (ch->runlist_id << 16) | (ch->chid & 0xFFFu);
        if (tok.work_submit_token != 0)
            cand[ncand++] = tok.work_submit_token | NV_DOORBELL_RUNLIST_ENABLE;
    }
    /* A class with no semaphore (currently NVDEC) only needs the authoritative
     * engine runlist and RM token rung.  Sweeping 64 candidates cannot improve
     * a status-buffer completion and cost 16.25 seconds on every decode. */
    u32 sweep_count = (!ch->rm->host_api && sem_value) ? 64u : 0u;
    for (u32 runl = 0; runl < sweep_count; runl++) {
        u32 db = NV_DOORBELL_RUNLIST_ENABLE | (runl << 16) | (ch->chid & 0xFFFu);
        if (ch->runlist_known && runl == ch->runlist_id) continue;   /* tried first */
        cand[ncand++] = db;
    }

    /* Host RM does not expose GP_GET, so its codec submissions cannot take
     * the fetched-ring grace period below. Give these bounded bring-up jobs
     * the Linux graphics-mode RM default (595 osGetTimeoutParams: 4 seconds).
     * This is an execution deadline, not a sleep or proof of codec correctness;
     * the exact completion cookie is still mandatory. Keep graphics/copy and
     * the legacy non-host doorbell policy unchanged. */
    bool host_codec = ch->rm->host_api &&
                      (ch == &channels[CH_NVENC] || ch == &channels[CH_NVDEC]);
    bool consumed = false;   /* latched: the card fetched the ring at least once */
    for (u32 ci = 0; ci < ncand; ci++) {
        u32 db = cand[ci];
        if (ch->rm->host_api) {
            *(volatile u32 *)(ch->rm->usermode + 0x90u) = db;
            __asm__ volatile("sfence" ::: "memory");
        } else {
            nv_wr32(ch->card, NV_USERMODE_DOORBELL, db);
        }
        u64 dl = g_uptime_ms + (host_codec ? 4000u : 250u);
        u64 spin_start = timer_now_us();
        while (g_uptime_ms < dl) {
            cache_flush((const void *)&ch->sem[0], 4);   /* sem is sysmem */
            if (nv_error_notification_failed(ch->error_notifier)) goto channel_error;
            /* Every caller that requests a semaphore uses a fresh per-channel
             * cookie.  Therefore this exact payload can only have been written
             * by this submission; GP_GET is a useful trace but is not a valid
             * veto on GB202 COPY4, where hardware proved its shadow can lag. */
            if (sem_value && ch->sem[0] == sem_value) {
                ch->submit_failed = false;
                if (trace_success)
                    kinfo("nv-chan", "completed: doorbell %#x ran the channel", db);
                return true;
            }
            if (!sem_value && userd_read_gpget(ch) == ch->gp_put) {
                ch->submit_failed = false;
                return true;
            }
            /* Latch consumption and log it ONCE.  The old code logged this from
             * inside the poll loop, so a genuinely-stuck semaphore reprinted the
             * same line every 50us - thousands per second, up to 1s per candidate
             * across ~66 candidates: a minute-long flood that evicted every other
             * real line from the log and read on screen as a hard freeze. */
            if (!consumed && userd_read_gpget(ch) == ch->gp_put) {
                consumed = true;
                kinfo("nv-chan", "doorbell %#x: card CONSUMED the ring (GP_GET=%u)"
                                 " - waiting for the engine to signal", db, ch->gp_put);
                if (!host_codec)
                    dl = g_uptime_ms + 1000; /* legacy fetched-ring grace */
            }
            /* Keep a short low-latency spin for tiny batches, then block the
             * pinned owner until its cookie or deadline. RM completion IRQs
             * probe promptly; the 1-kHz scheduler tick is the lost-IRQ fallback.
             * The outer loop still performs the authoritative retirement check.
             * Do not release the transaction or recycle buffers while asleep. */
            if (sem_value && timer_now_us() - spin_start >= 50u &&
                nv_completion_wait_allowed(ch)) {
                nv_completion_wait_t wait = {ch, sem_value};
                if (sched_wait_device(nv_completion_probe, &wait, dl) >= 0)
                    continue;
            }
            timer_udelay(ch->rm->host_api && !host_codec ? 2u : 50u);
        }
        cache_flush((const void *)&ch->sem[0], 4);
        if (nv_error_notification_failed(ch->error_notifier)) goto channel_error;
        if ((sem_value && ch->sem[0] == sem_value) ||
            (!sem_value && userd_read_gpget(ch) == ch->gp_put)) {
            ch->submit_failed = false;
            return true;
        }
        /* Once the card has fetched the ring, this doorbell is proven correct: a
         * still-silent semaphore is an ENGINE fault downstream of submit, not a
         * wrong doorbell.  Sweeping the remaining candidates cannot help and only
         * prolongs the flood - stop and report one clean verdict. */
        if (consumed) break;
    }

    u32 gp_get = userd_read_gpget(ch);
    if (ch->rm->host_api) {
        kwarn("nv-chan", "%s semaphore timeout (read %#x, wanted %#x); token %#x, GP_PUT %u; GP_GET unavailable under host RM, fetch progress and RC state unknown",
              ch->name, ch->sem[0], sem_value, tok.work_submit_token, ch->gp_put);
        nv_channel_capture_fault(ch, start_off, bytes, sem_value);
        return false;
    }
    kwarn("nv-chan", "the engine's semaphore did not fire (read %#x, wanted %#x); "
                     "token %#x, GP_GET %u of GP_PUT %u%s - %s",
          ch->sem[0], sem_value, tok.work_submit_token, gp_get, ch->gp_put,
          consumed ? "" : " after sweeping all runlists",
          gp_get == ch->gp_put ? "card CONSUMED the ring (pushbuffer/engine fault)"
                               : "card did NOT start at any runlist (USERD/schedule)");
    return false;

channel_error:
    /* An RM error is not a four-second semaphore timeout, and even a matching
     * cookie cannot make this channel reusable after RC. Keep all allocations
     * quarantined; the caller still captures firmware status/journal evidence. */
    kerr("nv-chan", "%s channel error while awaiting %#x (read %#x); RM error notifier published; submission quarantined",
          ch->name, sem_value, ch->sem[0]);
    nv_channel_capture_fault(ch, start_off, bytes, sem_value);
    return false;
}

/* Run a real copy on the card and verify it: fill a source region, clear a
 * destination, copy src->dst through the copy engine, and check both the
 * semaphore fired and the bytes arrived.  This is the end-to-end proof that
 * the channel + engine + doorbell + submit all work on the real GPU. */
static bool pb_reserve(nv_channel_t *ch, u32 need);
static bool pb_retire_and_wrap(nv_channel_t *ch);

int nv_chan_selftest(void) {
    nv_channel_t *ch = &channels[CH_COPY];
    if (!ch->open) { kwarn("nv-chan", "no channel to test"); return -1; }

    const u32 src_off = 0x8000, dst_off = 0xC000, len = 256;
    volatile u8 *src = ch->pushbuf + src_off;
    volatile u8 *dst = ch->pushbuf + dst_off;
    u64 src_va = VA_PUSHBUF + src_off, dst_va = VA_PUSHBUF + dst_off;

    for (u32 i = 0; i < len; i++) { src[i] = (u8)(0xA0 + i); dst[i] = 0; }
    ch->sem[0] = 0;
    __asm__ volatile("mfence" ::: "memory");

    if (!pb_reserve(ch, 96u)) return -1;
    u32 start = ch->pb_at;
    u32 signal = next_completion_signal(ch, 0x5a5a0000u);
    pb_method(ch, SUBCH_COPY, 0x0, 1); pb_data(ch, BLACKWELL_DMA_COPY_B);   /* SET_OBJECT */
    pb_method(ch, SUBCH_COPY, CE_OFFSET_IN_UPPER, 2);
        pb_data(ch, (u32)(src_va >> 32)); pb_data(ch, (u32)src_va);
    pb_method(ch, SUBCH_COPY, CE_OFFSET_OUT_UPPER, 2);
        pb_data(ch, (u32)(dst_va >> 32)); pb_data(ch, (u32)dst_va);
    pb_method(ch, SUBCH_COPY, CE_LINE_LENGTH_IN, 1); pb_data(ch, len);
    pb_method(ch, SUBCH_COPY, CE_LINE_COUNT, 1); pb_data(ch, 1);
    pb_method(ch, SUBCH_COPY, CE_SET_SEMAPHORE_A, 3);
        pb_data(ch, (u32)(VA_SEM >> 32)); pb_data(ch, (u32)VA_SEM); pb_data(ch, signal);
    pb_method(ch, SUBCH_COPY, CE_LAUNCH_DMA, 1);
        pb_data(ch, CE_LAUNCH | nv_completion_awaken(ch, CE_COMPLETION_AWAKEN));

    if (!submit_and_wait(ch, start, signal)) {
        kerr("nv-chan", "the copy engine did not signal completion");
        return -1;
    }
    __asm__ volatile("mfence" ::: "memory");
    for (u32 i = 0; i < len; i++) {
        if (dst[i] != (u8)(0xA0 + i)) {
            kerr("nv-chan", "the copy engine ran but byte %u is %#x, not %#x",
                 i, dst[i], (u8)(0xA0 + i));
            return -1;
        }
    }
    kinfo("nv-chan", "the copy engine COPIED %u bytes on the real card and "
                     "signalled done - hardware acceleration works", len);

    /* Do not let either a ring-wrap or idle-resume defect survive until visible
     * 3D.  Cross TWO complete production-size revolutions, then reproduce the exact
     * eight-second idle interval used by the visible 2D dwell and require one
     * more uniquely-fenced copy to resume.  The Linux-compatible RM timer and
     * submission ordering above must survive all of this before display tests
     * are allowed to begin. */
    const u32 proof_kickoffs = 2u * GPFIFO_ENTRIES + 5u;
    for (u32 kick = 0; kick < proof_kickoffs; kick++) {
        /* Exercise method-memory reuse before declaring desktop readiness,
         * not for the first time twenty seconds into an interactive session. */
        bool wrap_probe = kick == GPFIFO_ENTRIES / 2u ||
                          kick == GPFIFO_ENTRIES + GPFIFO_ENTRIES / 2u;
        if (wrap_probe) {
            if (!pb_retire_and_wrap(ch)) return -1;
            for (u32 i = 0; i < len; i++) dst[i] = 0;
            cache_flush(dst, len);
        }
        if (kick == proof_kickoffs - 1u) {
            kinfo("nv-chan", "GPFIFO idle/resume proof: waiting 8 seconds with the copy channel idle");
            timer_mdelay(8000);
        }
        if (!pb_reserve(ch, 96u)) return -1;
        signal = next_completion_signal(ch, 0x57520000u);
        ch->sem[0] = 0;
        __asm__ volatile("mfence" ::: "memory");
        start = ch->pb_at;
        pb_method(ch, SUBCH_COPY, 0x0, 1); pb_data(ch, BLACKWELL_DMA_COPY_B);
        pb_method(ch, SUBCH_COPY, CE_OFFSET_IN_UPPER, 2);
            pb_data(ch, (u32)(src_va >> 32)); pb_data(ch, (u32)src_va);
        pb_method(ch, SUBCH_COPY, CE_OFFSET_OUT_UPPER, 2);
            pb_data(ch, (u32)(dst_va >> 32)); pb_data(ch, (u32)dst_va);
        pb_method(ch, SUBCH_COPY, CE_LINE_LENGTH_IN, 1); pb_data(ch, len);
        pb_method(ch, SUBCH_COPY, CE_LINE_COUNT, 1); pb_data(ch, 1);
        pb_method(ch, SUBCH_COPY, CE_SET_SEMAPHORE_A, 3);
            pb_data(ch, (u32)(VA_SEM >> 32)); pb_data(ch, (u32)VA_SEM);
            pb_data(ch, signal);
        pb_method(ch, SUBCH_COPY, CE_LAUNCH_DMA, 1);
            pb_data(ch, CE_LAUNCH | nv_completion_awaken(ch, CE_COMPLETION_AWAKEN));
        if (!submit_and_wait(ch, start, signal)) {
            kerr("nv-chan", "GPFIFO wrap proof stopped at kickoff %u/%u (GP_PUT=%u)",
                 kick + 1u, proof_kickoffs, ch->gp_put);
            return -1;
        }
        if (wrap_probe) {
            cache_flush(dst, len);
            for (u32 i = 0; i < len; i++) {
                if (dst[i] != (u8)(0xA0 + i)) {
                    kerr("nv-chan", "HOST method-wrap copy readback failed at byte %u", i);
                    return -1;
                }
            }
        }
    }
    /* Initial copy + regular proof packets + two separate HOST packets. */
    if (ch->gp_put != 8u || ch->method_wraps != 2u) {
        kerr("nv-chan", "GPFIFO wrap arithmetic ended at %u, expected 8; method wraps %u, expected 2",
             ch->gp_put, ch->method_wraps);
        return -1;
    }
    kinfo("nv-chan", "GPFIFO wrap+idle PASS: %u synchronously fenced kickoffs crossed two complete revolutions and resumed after 8 seconds idle",
          proof_kickoffs);
    kinfo("nv-chan", "HOST method-wrap reuse PASS: two separate retirement barriers and byte-verified copies");
    return 0;
}

/* ------------------------------------------------ runtime scanout bridge ---
 * The boot tests proved that CAB5 can DMA from our coherent sysmem pushbuffer
 * to a GPU VA.  Keep a bounded staging window in that same mapping and use it
 * to upload desktop damage into the display's VRAM surface.  This makes the
 * live compositor/present path use the card; it is no longer a 256-byte test
 * that userland can never reach. */
/* Preserve a stable VA for every RM scanout allocation.  visible_accel_stage()
 * visits multiple heads and ping-pong slots; repeatedly replacing one PTE at a
 * single VA can leave an engine using a cached translation for the previous
 * monitor.  RM's physical FB offset is unique for the lifetime of an
 * allocation, so BASE+fb gives each surface a collision-free VA without a
 * remap/TLB-invalidate dependency.  Keep the scratch in a separate 1-TiB
 * window. */
#define VA_SCANOUT_BASE     0x0010000000000ull
#define VA_PRESENT_SCRATCH  0x0020000000000ull
#define VA_UPLOAD_STAGE     0x0021000000000ull
#define H_PRESENT_SCRATCH   0x004D0000u
#define PRESENT_STAGE_OFF   0x70000u
#define PRESENT_STAGE_BYTES 0x10000u
#define PRESENT_SCRATCH_BYTES 0x400000u
#define UPLOAD_STAGE_BYTES  0x400000u
#define SCANOUT_REGISTRY_MAX 8u
typedef struct {
    u64 fb, bytes, va;
    u32 pitch, width, height;
    bool compute_bound;
} runtime_scanout_t;
static bool g_scanout_bound;
static bool g_compute_scanout_bound;
static bool g_present_scratch_ready;
static bool g_upload_stage_attempted;
static bool g_upload_stage_mapped;
static bool g_upload_stage_ready;
static u64  g_present_scratch_fb;
static u64  g_upload_stage_phys;
static volatile u8 *g_upload_stage;
static u64  g_runtime_scanout_fb, g_runtime_scanout_bytes;
static u64  g_runtime_scanout_va;
static u32  g_runtime_scanout_pitch, g_runtime_scanout_width, g_runtime_scanout_height;
static runtime_scanout_t g_scanout_registry[SCANOUT_REGISTRY_MAX];
static u32 g_scanout_registry_count;

static void select_runtime_scanout(const runtime_scanout_t *s) {
    g_runtime_scanout_fb = s->fb;
    g_runtime_scanout_bytes = s->bytes;
    g_runtime_scanout_va = s->va;
    g_runtime_scanout_pitch = s->pitch;
    g_runtime_scanout_width = s->width;
    g_runtime_scanout_height = s->height;
    g_compute_scanout_bound = s->compute_bound;
    g_scanout_bound = true;
}

static bool pb_retire_and_wrap(nv_channel_t *ch) {
    if (ch->submit_failed) return false;
    /* NVIDIA's nvidia-push.c InsertProgressTracker()/IdleChannel(): put HOST
     * SEM_ADDR_LO..SEM_EXECUTE in a SEPARATE GPFIFO entry. With RELEASE_WFI
     * enabled it proves both HOST method consumption and downstream idle.
     * This synchronous client needs one retirement packet per wrap, rather
     * than one per asynchronous submission. Its storage is never part of the
     * method ring or CPU staging, and is not reused until the NEXT wrap. */
    const u32 old_end = ch->pb_at;
    ch->pb_at = PB_TRACKER_OFF;
    u32 signal = next_completion_signal(ch, 0x48540000u);
    ch->sem[0] = 0;
    __asm__ volatile("mfence" ::: "memory");
    pb_method(ch, 0, HOST_SEM_ADDR_LO, 5);
    pb_data(ch, (u32)VA_SEM); pb_data(ch, (u32)(VA_SEM >> 32));
    pb_data(ch, signal); pb_data(ch, 0u);
    pb_data(ch, HOST_SEM_RELEASE_WFI);
    if (!submit_and_wait(ch, PB_TRACKER_OFF, signal)) {
        kerr("nv-chan", "%s HOST retirement failed; method storage quarantined at %#x",
             ch->name, old_end);
        return false;
    }
    ch->pb_at = PB_METHOD_BEGIN;
    ch->method_wraps++;
    kinfo("nv-chan", "%s HOST-fenced method wrap %u: %#x -> %#x",
          ch->name, ch->method_wraps, old_end, ch->pb_at);
    return true;
}

static bool pb_reserve(nv_channel_t *ch, u32 need) {
    if (ch->submit_failed || !need || (need & 3u) ||
        need > PB_TRACKER_OFF - PB_METHOD_BEGIN ||
        ch->pb_at < PB_METHOD_BEGIN || ch->pb_at > PB_TRACKER_OFF)
        return false;
    if (need <= PB_TRACKER_OFF - ch->pb_at) return true;
    return pb_retire_and_wrap(ch);
}

bool nv_chan_bind_scanout(u64 fb, u64 bytes, u32 pitch, u32 width, u32 height) {
    nv_channel_t *copy = &channels[CH_COPY];
    nv_channel_t *comp = &channels[CH_GFX];
    if (!copy->open || copy->submit_failed || comp->submit_failed ||
        !fb || !bytes || !pitch || !width || !height) return false;
    for (u32 i = 0; i < g_scanout_registry_count; i++) {
        runtime_scanout_t *s = &g_scanout_registry[i];
        if (s->fb == fb && s->bytes == bytes && s->pitch == pitch &&
            s->width == width && s->height == height) {
            select_runtime_scanout(s);
            return true;
        }
    }
    /* Never let a failed rebind inherit readiness for the preceding head. */
    g_scanout_bound = false;
    g_compute_scanout_bound = false;
    u64 mapped = (bytes + PAGE_SIZE - 1u) & ~((u64)PAGE_SIZE - 1u);
    u64 scanout_va = VA_SCANOUT_BASE + fb;
    if (scanout_va < VA_SCANOUT_BASE || scanout_va + mapped < scanout_va ||
        scanout_va + mapped >= VA_PRESENT_SCRATCH) {
        kwarn("nv-chan", "runtime scanout address is outside its stable VA window");
        return false;
    }
    if (!nv_vmm_map(&copy->vmm, scanout_va, fb, mapped, true, false, false)) return false;
    /* Scanout-to-scanout moves need memmove semantics.  Keep the intermediate
     * entirely in VRAM: the former GPU->sysmem->GPU round trip depended on CPU
     * cache snooping for a GPU-written staging range and failed the live 2D
     * readback even though each completion semaphore fired. */
    if (!g_present_scratch_ready) {
        if (!nv_vram_alloc(copy->card, copy->rm, H_PRESENT_SCRATCH,
                           PRESENT_SCRATCH_BYTES, &g_present_scratch_fb)) {
            kwarn("nv-chan", "runtime present: no VRAM scratch for coherent 2D moves");
            return false;
        }
        g_present_scratch_ready = true;
    }
    if (!nv_vmm_map(&copy->vmm, VA_PRESENT_SCRATCH, g_present_scratch_fb,
                    PRESENT_SCRATCH_BYTES, true, false, false)) {
        kwarn("nv-chan", "runtime present: could not map VRAM move scratch");
        return false;
    }
    /* A four-megabyte coherent upload window turns a 1440p desktop present
     * from hundreds of tiny GPFIFO entries into four bounded DMA submissions.
     * Fall back to the pushbuffer's 64-KiB tail if contiguous RAM is scarce. */
    if (!g_upload_stage_attempted) {
        g_upload_stage_attempted = true;
        g_upload_stage = dma_alloc_pages(UPLOAD_STAGE_BYTES / PAGE_SIZE,
                                         &g_upload_stage_phys);
        if (g_upload_stage &&
            nv_vmm_map(&copy->vmm, VA_UPLOAD_STAGE, g_upload_stage_phys,
                       UPLOAD_STAGE_BYTES, false, false, false)) {
            g_upload_stage_mapped = true;
        } else {
            kwarn("nv-chan", "runtime present: 4-MiB DMA stage unavailable; using 64-KiB fallback");
            /* A failed map may leave page-table references to part of this
             * allocation. Retain it and do not allocate another stage on each
             * display rebind; it is not a safe free/reuse candidate. */
        }
    }
    g_upload_stage_ready = false;
    if (!nv_vmm_commit(copy->card, copy->rm, &copy->vmm,
                       h_vaspace(CH_COPY), "runtime copy scanout")) {
        kwarn("nv-chan", "runtime present: copy-channel VMM commit failed");
        return false;
    }
    /* Shadow mapping is not hardware readiness. Publish only after the actual
     * COPY tables and TLB invalidation have committed successfully. */
    g_upload_stage_ready = g_upload_stage_mapped;
    if (comp->open) {
        if (!nv_vmm_map(&comp->vmm, scanout_va, fb, mapped, true, false, false)) {
            kwarn("nv-chan", "scanout mapped for copy/present but not compute/3D");
        } else {
            if (!nv_vmm_commit(comp->card, comp->rm, &comp->vmm,
                               h_vaspace(CH_GFX), "runtime GR scanout")) {
                kwarn("nv-chan", "scanout compute/3D VMM commit failed");
            } else {
                g_compute_scanout_bound = true;
            }
        }
    }
    runtime_scanout_t now = {
        .fb = fb, .bytes = bytes, .va = scanout_va,
        .pitch = pitch, .width = width, .height = height,
        .compute_bound = g_compute_scanout_bound,
    };
    if (g_scanout_registry_count < SCANOUT_REGISTRY_MAX)
        g_scanout_registry[g_scanout_registry_count++] = now;
    else
        kwarn("nv-chan", "runtime scanout registry full; this surface will require a later VMM rebind");
    select_runtime_scanout(&now);
    kinfo("nv-chan", "runtime GPU scanout bound: FB %#llx -> VA %#llx, %ux%u pitch %u",
          (unsigned long long)fb, (unsigned long long)scanout_va,
          width, height, pitch);
    kinfo("nv-chan", "runtime 2D move scratch: FB %#llx -> VA %#llx (%#x bytes)",
          (unsigned long long)g_present_scratch_fb,
          (unsigned long long)VA_PRESENT_SCRATCH, PRESENT_SCRATCH_BYTES);
    return true;
}

bool nv_chan_display_ready(void) {
    return g_scanout_bound && channels[CH_COPY].open &&
           !channels[CH_COPY].submit_failed;
}

nv_card_t *nv_chan_display_card(void) {
    return nv_chan_display_ready() ? channels[CH_COPY].card : NULL;
}

bool nv_chan_raster_ready(nv_card_t *card) {
    nv_channel_t *ch = &channels[CH_GFX];
    return card && nv_chan_display_card() == card && ch->card == card &&
           ch->open && !ch->submit_failed && g_compute_scanout_bound;
}

static bool ce_copy(nv_channel_t *ch, u64 src, u64 dst, u32 src_pitch,
                    u32 dst_pitch, u32 line_bytes, u32 lines, u32 signal) {
    if (!line_bytes || !lines) return true;
    if (!pb_reserve(ch, 96)) return false;
    signal = next_completion_signal(ch, signal);
    ch->sem[0] = 0;
    __asm__ volatile("mfence" ::: "memory");
    u32 start = ch->pb_at;
    pb_method(ch, SUBCH_COPY, 0x0, 1); pb_data(ch, BLACKWELL_DMA_COPY_B);
    pb_method(ch, SUBCH_COPY, CE_OFFSET_IN_UPPER, 2);
        pb_data(ch, (u32)(src >> 32)); pb_data(ch, (u32)src);
    pb_method(ch, SUBCH_COPY, CE_OFFSET_OUT_UPPER, 2);
        pb_data(ch, (u32)(dst >> 32)); pb_data(ch, (u32)dst);
    pb_method(ch, SUBCH_COPY, CE_PITCH_IN, 2);
        pb_data(ch, src_pitch); pb_data(ch, dst_pitch);
    pb_method(ch, SUBCH_COPY, CE_LINE_LENGTH_IN, 2);
        pb_data(ch, line_bytes); pb_data(ch, lines);
    pb_method(ch, SUBCH_COPY, CE_SET_SEMAPHORE_A, 3);
        pb_data(ch, (u32)(VA_SEM >> 32)); pb_data(ch, (u32)VA_SEM); pb_data(ch, signal);
    pb_method(ch, SUBCH_COPY, CE_LAUNCH_DMA, 1);
        pb_data(ch, CE_LAUNCH | nv_completion_awaken(ch, CE_COMPLETION_AWAKEN));
    return submit_and_wait(ch, start, signal);
}

/* Generate pixels on the CE, with no CPU pixel tile or source DMA read.
 * Remap changes LINE_LENGTH_IN to elements (u32 pixels), NOT bytes; pitches
 * remain bytes. Multi-line remap follows Mesa nvk_cmd_fill_memory_ce, with
 * our existing virtual-address, PLC-disable and completion-fence contract.
 * ce_copy() always emits REMAP_ENABLE=FALSE so this state cannot leak into
 * the next image upload or VRAM copy. */
static bool ce_fill32(nv_channel_t *ch, u64 dst, u32 pitch,
                      u32 width, u32 height, u32 colour, u32 signal) {
    if (!ch->open || ch->submit_failed || !width || !height ||
        width > 32768u || height > 32768u || (dst & 3u) || (pitch & 3u) ||
        (u64)width * 4u > pitch) return false;
    u64 bytes = (u64)(height - 1u) * pitch + (u64)width * 4u;
    if (dst + bytes < dst || (dst + bytes - 1u) >> 49) return false;
    if (!pb_reserve(ch, 80u)) return false;
    signal = next_completion_signal(ch, signal);
    ch->sem[0] = 0;
    __asm__ volatile("mfence" ::: "memory");
    u32 start = ch->pb_at;
    pb_method(ch, SUBCH_COPY, 0x0, 1); pb_data(ch, BLACKWELL_DMA_COPY_B);
    pb_method(ch, SUBCH_COPY, CE_SET_REMAP_CONST_B, 2);
        pb_data(ch, colour); pb_data(ch, CE_REMAP_FILL32);
    pb_method(ch, SUBCH_COPY, CE_OFFSET_OUT_UPPER, 2);
        pb_data(ch, (u32)(dst >> 32)); pb_data(ch, (u32)dst);
    pb_method(ch, SUBCH_COPY, CE_PITCH_IN, 2);
        pb_data(ch, width * 4u); pb_data(ch, pitch);
    pb_method(ch, SUBCH_COPY, CE_LINE_LENGTH_IN, 2);
        pb_data(ch, width); pb_data(ch, height);
    pb_method(ch, SUBCH_COPY, CE_SET_SEMAPHORE_A, 3);
        pb_data(ch, (u32)(VA_SEM >> 32)); pb_data(ch, (u32)VA_SEM); pb_data(ch, signal);
    pb_method(ch, SUBCH_COPY, CE_LAUNCH_DMA, 1);
        pb_data(ch, CE_LAUNCH | CE_REMAP_ENABLE |
                    nv_completion_awaken(ch, CE_COMPLETION_AWAKEN));
    return submit_and_wait(ch, start, signal);
}

typedef struct {
    u64 src,dst;
    u32 src_pitch,dst_pitch,line,lines;
    bool fill;
} nv_ce_prepare_t;

/* A small, prevalidated preparation packet, not an asynchronous public API.
 * The official hopper CE HAL (inherited by Blackwell) emits ordinary DMA
 * operations without SEMAPHORE_TYPE, then releases at the end of the push.
 * Keep our proven NON_PIPELINED + SYS flush on EVERY operation; only the last
 * one publishes the unique completion cookie/interrupt. Thus a CPU wake can
 * never mistake an intermediate clear for completion of the following copy.
 * Reserve the entire packet before emitting anything: no mid-packet wrap. */
static bool ce_prepare_batch(nv_channel_t *ch,const nv_ce_prepare_t *ops,u32 count) {
    if(!ch || !ch->open || ch->submit_failed || !ops || !count || count>3u)return false;
    for(u32 i=0;i<count;i++) {
        const nv_ce_prepare_t *p=&ops[i];
        if(!p->line || !p->lines || p->lines>32768u ||
           ((p->dst|p->src_pitch|p->dst_pitch)&3u))return false;
        u64 row=p->fill?(u64)p->line*4u:p->line;
        if((p->fill&&p->line>32768u) || (!p->fill&&((p->src|p->line)&3u)) ||
           row>p->src_pitch || row>p->dst_pitch)return false;
        u64 span=(u64)(p->lines-1u)*p->dst_pitch+row;
        if(p->dst+span<p->dst || (p->dst+span-1u)>>49)return false;
        span=(u64)(p->lines-1u)*p->src_pitch+row;
        if(!p->fill&&(p->src+span<p->src || (p->src+span-1u)>>49))return false;
    }
    if(!pb_reserve(ch,96u*count))return false;
    u32 signal=next_completion_signal(ch,0x42500000u),start=ch->pb_at;
    ch->sem[0]=0;__asm__ volatile("mfence" ::: "memory");
    pb_method(ch,SUBCH_COPY,0x0,1);pb_data(ch,BLACKWELL_DMA_COPY_B);
    for(u32 i=0;i<count;i++) {
        const nv_ce_prepare_t *p=&ops[i];bool last=i+1u==count;
        if(p->fill) {
            pb_method(ch,SUBCH_COPY,CE_SET_REMAP_CONST_B,2);
            pb_data(ch,0u);pb_data(ch,CE_REMAP_FILL32);
        } else {
            pb_method(ch,SUBCH_COPY,CE_OFFSET_IN_UPPER,2);
            pb_data(ch,(u32)(p->src>>32));pb_data(ch,(u32)p->src);
        }
        pb_method(ch,SUBCH_COPY,CE_OFFSET_OUT_UPPER,2);
        pb_data(ch,(u32)(p->dst>>32));pb_data(ch,(u32)p->dst);
        pb_method(ch,SUBCH_COPY,CE_PITCH_IN,2);
        pb_data(ch,p->src_pitch);pb_data(ch,p->dst_pitch);
        pb_method(ch,SUBCH_COPY,CE_LINE_LENGTH_IN,2);
        pb_data(ch,p->line);pb_data(ch,p->lines);
        if(last) {
            pb_method(ch,SUBCH_COPY,CE_SET_SEMAPHORE_A,3);
            pb_data(ch,(u32)(VA_SEM>>32));pb_data(ch,(u32)VA_SEM);pb_data(ch,signal);
        }
        u32 launch=CE_LAUNCH & ~(3u<<3); /* no intermediate semaphore release */
        if(p->fill)launch|=CE_REMAP_ENABLE;
        if(last)launch|=(1u<<3)|nv_completion_awaken(ch,CE_COMPLETION_AWAKEN);
        pb_method(ch,SUBCH_COPY,CE_LAUNCH_DMA,1);pb_data(ch,launch);
    }
    return submit_and_wait(ch,start,signal);
}

bool nv_chan_present_image(const u32 *pixels, u32 width, u32 height,
                           u32 source_stride, s32 x, s32 y) {
    nv_channel_t *ch = &channels[CH_COPY];
    if (!nv_chan_display_ready() || !pixels || !width || !height) return false;
    if (!source_stride) source_stride = width;
    if (source_stride < width || x < 0 || y < 0 ||
        (u64)(u32)x + width > g_runtime_scanout_width ||
        (u64)(u32)y + height > g_runtime_scanout_height) return false;
    u32 row_bytes = width * 4u;
    u32 stage_bytes = g_upload_stage_ready ? UPLOAD_STAGE_BYTES
                                           : PRESENT_STAGE_BYTES;
    volatile u8 *stage = g_upload_stage_ready
                       ? g_upload_stage
                       : ch->pushbuf + PRESENT_STAGE_OFF;
    u64 stage_va = g_upload_stage_ready ? VA_UPLOAD_STAGE
                                        : VA_PUSHBUF + PRESENT_STAGE_OFF;
    if (!row_bytes || row_bytes > stage_bytes) return false;
    u32 max_rows = stage_bytes / row_bytes;
    if (!max_rows) max_rows = 1;
    for (u32 top = 0; top < height; ) {
        u32 rows = height - top; if (rows > max_rows) rows = max_rows;
        for (u32 r = 0; r < rows; r++)
            memcpy((void *)(stage + (u64)r * row_bytes),
                   pixels + (u64)(top + r) * source_stride, row_bytes);
        cache_flush(stage, row_bytes * rows);
        u64 dst = g_runtime_scanout_va +
                  (u64)(y + (s32)top) * g_runtime_scanout_pitch + (u64)x * 4u;
        if (!ce_copy(ch, stage_va, dst,
                     row_bytes, g_runtime_scanout_pitch, row_bytes, rows,
                     0x50520000u | ((top / max_rows) & 0xffffu))) return false;
        top += rows;
    }
    return true;
}

bool nv_chan_fill_scanout(s32 x, s32 y, s32 w, s32 h, u32 colour) {
    if (!nv_chan_display_ready() || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        (u64)(u32)x + (u32)w > g_runtime_scanout_width ||
        (u64)(u32)y + (u32)h > g_runtime_scanout_height) return false;
    u64 base = g_runtime_scanout_va +
               (u64)y * g_runtime_scanout_pitch + (u64)x * 4u;
    return ce_fill32(&channels[CH_COPY], base, g_runtime_scanout_pitch,
                     (u32)w, (u32)h, colour, 0x46490000u);
}

bool nv_chan_copy_scanout(s32 sx, s32 sy, s32 dx, s32 dy, s32 w, s32 h) {
    if (!nv_chan_display_ready() || sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
        w <= 0 || h <= 0 || (u64)(u32)sx + (u32)w > g_runtime_scanout_width ||
        (u64)(u32)dx + (u32)w > g_runtime_scanout_width ||
        (u64)(u32)sy + (u32)h > g_runtime_scanout_height ||
        (u64)(u32)dy + (u32)h > g_runtime_scanout_height) return false;
    /* CAB5 overlap order is not controllable here.  Preserve memmove semantics
     * by staging through a dedicated VRAM surface in bounded bands. */
    u32 row_bytes = (u32)w * 4u;
    if (!g_present_scratch_ready || row_bytes > PRESENT_SCRATCH_BYTES) return false;
    nv_channel_t *ch = &channels[CH_COPY];
    u32 rows = PRESENT_SCRATCH_BYTES / row_bytes; if (!rows) rows = 1;
    s32 top = dy > sy ? h : 0;
    while ((dy > sy && top > 0) || (dy <= sy && top < h)) {
        u32 n;
        if (dy > sy) {
            n = (u32)top; if (n > rows) n = rows;
            top -= (s32)n;
        } else {
            n = (u32)(h - top); if (n > rows) n = rows;
        }
        u64 src = g_runtime_scanout_va +
                  (u64)(sy + top) * g_runtime_scanout_pitch + (u64)sx * 4u;
        if (!ce_copy(ch, src, VA_PRESENT_SCRATCH,
                      g_runtime_scanout_pitch, row_bytes, row_bytes, n, 0x43505231u)) return false;
        u64 dst = g_runtime_scanout_va +
                  (u64)(dy + top) * g_runtime_scanout_pitch + (u64)dx * 4u;
        if (!ce_copy(ch, VA_PRESENT_SCRATCH, dst,
                      row_bytes, g_runtime_scanout_pitch, row_bytes, n, 0x43505232u)) return false;
        if (dy <= sy) top += (s32)n;
    }
    return true;
}

/* ------------------------------------------------ compute dispatch (real HW) */

/* BLACKWELL_COMPUTE_B (0xcec0) methods: the 3-method launch (verified in
 * nv_compute.c against clcec0.h) plus a completion report.  WAIT_FOR_IDLE
 * stalls method processing until the async grid is idle, then the report
 * semaphore releases a payload to a VA - the same completion mechanism the
 * copy engine's semaphore uses, so submit_and_wait works unchanged. */
#define CEC0_SET_OBJECT          0x0000u
#define CEC0_WAIT_FOR_IDLE       0x0110u
#define CEC0_LINE_LENGTH_IN     0x0180u
#define CEC0_LAUNCH_DMA         0x01b0u
#define CEC0_INLINE_DMA_FLUSH   0x11u /* PITCH | COMPLETION_TYPE_FLUSH_ONLY */
/* Mandatory per-context compute init (verified vs NVK nvk_push_dispatch_state_init
 * + clcec0.h): the SM cannot resolve local/shared addresses until these windows
 * are set, so a dispatch faults BEFORE it runs - a consumed ring with a silent
 * completion semaphore, exactly what the first hardware boot showed. */
#define CEC0_INVALIDATE_SKED_CACHES              0x0298u
#define CEC0_INVALIDATE_SHADER_CACHES            0x021cu
/* CEC0: INSTRUCTION bit 0, DATA bit 4, CONSTANT bit 12. */
#define CEC0_INVALIDATE_SHADER_CODE_DATA_CONST   0x1011u
#define CEC0_SET_SHADER_SHARED_MEMORY_WINDOW_A   0x02a0u   /* +0x02a4 = _B */
#define CEC0_SET_SHADER_LOCAL_MEMORY_WINDOW_A    0x07b0u   /* +0x07b4 = _B */
#define CEC0_REPORT_SEM_PAY_LO   0x0158u
#define CEC0_REPORT_SEM_ADDR_LO  0x0160u   /* +0x0164 = ADDR_UPPER (auto-inc)   */
#define CEC0_REPORT_SEM_EXECUTE  0x0168u
#define CEC0_SEND_PCAS_A         0x02b4u   /* QMD address >> 8                  */
#define CEC0_SEND_PCAS2_B        0x02c0u
#define CEC0_PCAS2_ACTION_ICS    0x3u      /* INVALIDATE_COPY_SCHEDULE          */
/* EXECUTE: OPERATION_RELEASE(0) | STRUCTURE_SIZE_ONE_WORD(1<<3); FLUSH_DISABLE
 * left 0 so the kernel's writes are pushed to memory before the release. */
#define CEC0_REPORT_SEM_RELEASE_1W  (0x1u << 3)
/* Compute (0xCEC0) MUST bind to subchannel 1 - the GR engine has FIXED
 * subchannel-to-class slots (3D=0, COMPUTE=1, M2MF=2, 2D=3, COPY=4; Mesa
 * nv_push.h: SUBC_NVC6C0=1).  On subchannel 0 (the 3D slot) GSP-RM raised
 * "Graphics Exception: Class 0xcec0 Subchannel 0x0 Mismatch" (Xid 13) and no SM
 * ever ran the grid - the root cause of the whole compute wall. */
#define SUBCH_COMPUTE            1

/* Shader code, QMD, and the constant bank MUST be VRAM-resident.  SKED fetches
 * the QMD (SEND_PCAS_A) and the SM fetches instructions + constants through the
 * GPU MMU + caches, which - unlike the copy engine's PCIe DMA - cannot service a
 * sysmem/SYSCOH aperture: a sysmem QMD/shader is admitted by CWD (topology is
 * present) but NO SM ever launches and the GSP watchdog RCs the channel.  Every
 * reference co-locates these in VRAM (NVK shader_heap/QMD/cbuf = NVKMD_MEM_LOCAL,
 * nvk_device.c:336 / nvk_cmd_pool.c:24; OGKM compute object = ADDR_FBMEM,
 * kernel_graphics_object.c:428) and puts only the pushbuffer in sysmem.  We build
 * in the sysmem pushbuffer (staging), copy into this VRAM buffer via PRAMIN, and
 * point PROGRAM_ADDRESS/SEND_PCAS/cbuf at the VRAM VAs. */
#define VA_COMPUTE          0x00500000000ull
#define H_COMPUTE_VRAM      0x00490000u
#define NV_COMPUTE_VRAM_BYTES 0x90000u
/* Keep the original low-64KiB scratch/legacy layout. Immutable desktop and
 * application programs live above it, outside every command/VM upload. */
#define NV_GUI_RESIDENT_OFF 0x10000u
#define NV_PRESENT_RESIDENT_OFF 0x18000u
#define NV_GL_RESIDENT_OFF 0x20000u
#define NV_VM_RESIDENT_OFF 0x30000u
#define NV_RASTER_RESIDENT_OFF 0x40000u
#define NV_RASTER_FAST_RESIDENT_OFF 0x60000u
#define NV_SETUP_RESIDENT_OFF 0x70000u
#define NV_COMPACT_RESIDENT_OFF 0x80000u
#define NV_COMPACT_SMALL_RESIDENT_OFF 0x88000u
#define NV_RESIDENT_PROGRAMS 9u
_Static_assert(sizeof gui_raster_sass <= 0x8000u, "resident widget program slot");
_Static_assert(sizeof gui_present_sass <= 0x8000u, "resident present program slot");
_Static_assert(sizeof gl_raster_sass <= 0x10000u, "resident fixed 3D program slot");
_Static_assert(sizeof shader_vm_sass <= 0x10000u, "resident vertex VM program slot");
_Static_assert(sizeof shader_raster_sass <= 0x20000u, "resident fragment program slot");
_Static_assert(sizeof shader_raster_fast_sass <= 0x10000u, "resident register fragment program slot");
_Static_assert(sizeof shader_setup_sass <= 0x10000u, "resident triangle setup slot");
_Static_assert(sizeof shader_setup_compact_sass <= 0x8000u, "resident cooperative compaction slot");
_Static_assert(sizeof shader_setup_compact_small_sass <= 0x8000u, "resident tiny compaction slot");
_Static_assert(NV_COMPACT_RESIDENT_OFF+0x8000u == NV_COMPACT_SMALL_RESIDENT_OFF, "compaction slot separation");
_Static_assert(NV_COMPACT_RESIDENT_OFF+0x10000u == NV_COMPUTE_VRAM_BYTES, "resident arena end");
static u64  g_compute_vram_fb;
static bool g_compute_vram_ready;
static bool g_compute_vram_attempted;
static bool g_compute_copy_map_attempted;
static bool g_compute_copy_map_ready;
static bool nv_surface_transfer_ce_ready(void);
static bool nv_runtime_copy_transfer(u64 gpu, void *data, u32 bytes, bool read);

static bool ensure_compute_vram(nv_channel_t *ch) {
    if (g_compute_vram_ready) return true;
    /* An allocation/map failure may leave partial RM state under this fixed
     * handle. Do not retry allocation over it without verified teardown. */
    if (g_compute_vram_attempted) return false;
    g_compute_vram_attempted = true;
    if (!nv_vram_alloc(ch->card, ch->rm, H_COMPUTE_VRAM, NV_COMPUTE_VRAM_BYTES, &g_compute_vram_fb)) {
        kwarn("nv-chan", "compute: no VRAM for the shader/QMD buffer"); return false;
    }
    if (!nv_vmm_map(&ch->vmm, VA_COMPUTE, g_compute_vram_fb, NV_COMPUTE_VRAM_BYTES, true, false, false)) {
        kwarn("nv-chan", "compute: could not map the VRAM shader/QMD buffer"); return false;
    }
    if (!nv_vmm_commit(ch->card, ch->rm, &ch->vmm,
                       h_vaspace(CH_GFX), "compute work buffer")) {
        kwarn("nv-chan", "compute: coherent VMM commit failed");
        return false;
    }
    kinfo("nv-chan", "compute: shader/QMD VRAM buffer at fb %#llx mapped at VA %#llx",
          (unsigned long long)g_compute_vram_fb, (unsigned long long)VA_COMPUTE);
    g_compute_vram_ready = true;
    return true;
}

/* Private launch/program storage is never a user surface. Runtime uploads may
 * use COPY only after the SAME linear mapping used by GR is committed in COPY.
 * The old coherent RM path remains the pre-runtime/boot implementation. */
static bool nv_compute_copy_mapping(nv_channel_t *ch) {
    nv_channel_t *copy = &channels[CH_COPY];
    if (!g_compute_vram_ready || !nv_surface_transfer_ce_ready() ||
        ch->submit_failed || copy->submit_failed) return false;
    if (g_compute_copy_map_attempted && !g_compute_copy_map_ready) return false;
    if (!g_compute_copy_map_ready) {
        g_compute_copy_map_attempted = true;
        if (!nv_vmm_map(&copy->vmm, VA_COMPUTE, g_compute_vram_fb,
                         NV_COMPUTE_VRAM_BYTES, true, false, false) ||
            !nv_vmm_commit(copy->card, copy->rm, &copy->vmm,
                            h_vaspace(CH_COPY), "compute COPY upload mapping")) {
            kerr("nv-chan", "compute COPY upload mapping failed; retained, no DMA/retry");
            return false;
        }
        g_compute_copy_map_ready = true;
    }
    return true;
}

static bool nv_compute_upload(nv_channel_t *ch, u64 offset, const void *data, u64 bytes) {
    if (!g_compute_vram_ready || !data || ((offset | bytes) & 3u) ||
        offset > NV_COMPUTE_VRAM_BYTES || bytes > NV_COMPUTE_VRAM_BYTES-offset ||
        ch->submit_failed || channels[CH_COPY].submit_failed) return false;
    if (g_compute_copy_map_attempted && !g_compute_copy_map_ready) return false;
    if (!nv_surface_transfer_ce_ready())
        return nv_vram_object_write(ch, H_COMPUTE_VRAM, g_compute_vram_fb,
                                    offset, data, bytes);
    if (!nv_compute_copy_mapping(ch)) return false;
    /* A failed DMA is terminal here: never retry this upload through MemUtils. */
    return nv_runtime_copy_transfer(VA_COMPUTE+offset, (void *)data, (u32)bytes, false);
}

typedef struct {
    u32 qmd_off, shader_off, constant_off, bytes;
} nv_dispatch_layout_t;

/* Preserve the proven layout for <=1KiB kernels, but never place constants
 * over a larger shader. All dispatch storage must remain in the private
 * low-64KiB staging/VRAM allocation, below the method ring. */
static bool nv_compute_dispatch_layout(u32 data_off, u32 sass_len,
                                       u32 args_words, nv_dispatch_layout_t *out) {
    if (!out || (data_off & 0xffu) || data_off > 0x10000u ||
        !sass_len || (sass_len & 15u) || sass_len > 0xf800u ||
        args_words > 0x20u) return false;
    u32 shader = data_off + 0x400u;
    u32 constants = (shader + sass_len + 0x3ffu) & ~0x3ffu;
    if (constants < data_off + 0x800u) constants = data_off + 0x800u;
    u32 end = constants + 0x400u;
    if (end > 0x10000u) return false;
    *out = (nv_dispatch_layout_t){data_off, shader, constants, end - data_off};
    return true;
}

/* Resident code lives in a separate, statically bounded immutable slot. Only
 * QMD/bank2 + bank0 occupy low staging memory; do not charge the program bytes
 * against that unrelated 64-KiB arena. The caller selects an exact asset/size
 * and supplies its checked resident offset after this dynamic-only layout. */
static bool nv_compute_resident_layout(u32 data_off, u32 args_words,
                                       nv_dispatch_layout_t *out) {
    if (!out || (data_off & 0xffu) || data_off > 0xf800u || args_words > 0x20u)
        return false;
    *out = (nv_dispatch_layout_t){data_off, 0u, data_off + 0x400u, 0x800u};
    return true;
}

/* Caller reserves the whole launch before emitting any methods. Runtime
 * resident launches have exactly 2 KiB of changing QMD/bank2/bank0 data in
 * the private low-64KiB arena. Place it inline on the SAME GR channel as PCAS:
 * no COPY submission or CPU wake is needed for this upload. NVIDIA CEC0 I2M
 * PITCH + FLUSH_ONLY keeps SYSMEMBAR enabled. Mesa's
 * nvk_cmd_compute_indirect_copy uses this sequence for QMD/root updates before
 * INVALIDATE_COPY_SCHEDULE. One-increment advances from LAUNCH_DMA to
 * LOAD_INLINE_DATA once, then holds that method for the entire payload.
 * Retirement is still the following compute WFI + flushed semaphore release. */
static bool nv_compute_inline_upload(nv_channel_t *ch,u32 offset,
                                     const volatile u8 *data,u32 bytes) {
    if(!ch || !ch->open || ch->submit_failed || !data ||
       ((u64)data&3u) || (offset&0xffu) || offset>0xf800u || bytes!=0x800u)
        return false;
    u64 destination=VA_COMPUTE+offset;
    pb_method(ch,SUBCH_COMPUTE,CEC0_LINE_LENGTH_IN,4u);
    pb_data(ch,bytes);pb_data(ch,1u);
    pb_data(ch,(u32)(destination>>32));pb_data(ch,(u32)destination);
    pb_data(ch,(5u<<29)|(((bytes/4u)+1u)<<16)|
               (SUBCH_COMPUTE<<13)|(CEC0_LAUNCH_DMA>>2));
    pb_data(ch,CEC0_INLINE_DMA_FLUSH);
    for(u32 i=0;i<bytes;i+=4u)pb_data(ch,*(const volatile u32 *)(data+i));
    return true;
}

/* Run ONE real compute grid on the shared CH_GFX CEC0 subchannel: place the shader `sass` and a
 * zeroed constant bank 0 (with `arg_va` at CUDA's arg offset 0x380, if given)
 * and a QMD in this channel's pushbuffer region (all mapped at VA_PUSHBUF), then
 * SET_OBJECT / SEND_PCAS_A / PCAS2_B to dispatch, WAIT_FOR_IDLE, and release
 * `sentinel` to VA_SEM.  Returns true if the semaphore fired = the grid ran to
 * completion on the real card. data_off selects a bounded, non-overlapping
 * QMD/program/constant layout sized from the actual shader. */
typedef struct {
    u32 start, end, recorded, signal;
} nv_compute_pair_t;

static bool nv_compute_dispatch_record(nv_channel_t *ch,
                                    const unsigned char *sass, u32 sass_len,
                                    u32 reg_count, u32 data_off,
                                    const u32 *args, u32 args_words,
                                    const u32 *grid, const u32 *block,
                                    u32 sentinel, const char *label,
                                    nv_compute_pair_t *pair) {
    nv_dispatch_layout_t layout;
    if (!ch || !ch->open || ch->submit_failed || !sass ||
        !sass_len || (sass_len & 15u) ||
        (args_words && !args) || !reg_count || reg_count > 255u)
        return false;
    /* Only these exact immutable kernel assets use residency. Other shaders
     * retain the existing low-64KiB upload/layout, including boot triangles.
     * Their writes cannot reach any resident slot. Runtime bytecode, uniforms,
     * descriptors and branch-table bindings are still refreshed each launch. */
    u32 resident = sass == gui_raster_sass && sass_len == sizeof gui_raster_sass ? 0u :
                   sass == gui_present_sass && sass_len == sizeof gui_present_sass ? 1u :
                   sass == gl_raster_sass && sass_len == sizeof gl_raster_sass ? 2u :
                   sass == shader_vm_sass && sass_len == sizeof shader_vm_sass ? 3u :
                   sass == shader_raster_sass && sass_len == sizeof shader_raster_sass ? 4u :
                   sass == shader_raster_fast_sass && sass_len == sizeof shader_raster_fast_sass ? 5u :
                   sass == shader_setup_sass && sass_len == sizeof shader_setup_sass ? 6u :
                   sass == shader_setup_compact_sass && sass_len == sizeof shader_setup_compact_sass ? 7u :
                   sass == shader_setup_compact_small_sass && sass_len == sizeof shader_setup_compact_small_sass ? 8u : NV_RESIDENT_PROGRAMS;
    static const u32 resident_offsets[NV_RESIDENT_PROGRAMS] = {NV_GUI_RESIDENT_OFF, NV_PRESENT_RESIDENT_OFF,
        NV_GL_RESIDENT_OFF, NV_VM_RESIDENT_OFF, NV_RASTER_RESIDENT_OFF, NV_RASTER_FAST_RESIDENT_OFF,
        NV_SETUP_RESIDENT_OFF, NV_COMPACT_RESIDENT_OFF, NV_COMPACT_SMALL_RESIDENT_OFF};
    static bool resident_program_loaded[NV_RESIDENT_PROGRAMS];
    if (resident < NV_RESIDENT_PROGRAMS) {
        if (!nv_compute_resident_layout(data_off, args_words, &layout)) return false;
        layout.shader_off = resident_offsets[resident];
    } else if (!nv_compute_dispatch_layout(data_off, sass_len, args_words, &layout))
        return false;
    const u32 qmd_off = layout.qmd_off;
    const u32 sh_off  = layout.shader_off;
    const u32 cb_off  = layout.constant_off;
    volatile u8 *base = ch->pushbuf;        /* sysmem STAGING (built here first) */
    /* The addresses the GPU uses are in VRAM (ensure_compute_vram), NOT sysmem. */
    u64 qmd_va = VA_COMPUTE + qmd_off;
    u64 sh_va  = VA_COMPUTE + sh_off;
    u64 cb_va  = VA_COMPUTE + cb_off;

    if (!ensure_compute_vram(ch)) return false;
    bool inline_launch=resident<NV_RESIDENT_PROGRAMS && nv_surface_transfer_ce_ready();
    /* Inline upload adds 28 bytes of methods plus 2KiB payload to the existing
     * <=128-byte dispatch. No ring wrap may occur within this combined packet. */
    if (pair) {
        /* Only the private two-grid helper can defer publication. It reserves
         * the COMPLETE packet before recording; no wrap or host submission is
         * allowed between its grids. Separate QMD/CB slots stay immutable. */
        if (!g_render_transaction || !inline_launch || pair->recorded>=2u ||
            data_off!=(pair->recorded?0xc00u:0x400u) ||
            ch->pb_at<pair->start || ch->pb_at>pair->end ||
            layout.bytes+160u>pair->end-ch->pb_at) return false;
    } else if (!pb_reserve(ch, inline_launch?layout.bytes+160u:128u)) return false;
    if (resident < NV_RESIDENT_PROGRAMS && !resident_program_loaded[resident]) {
        /* Publish residency only after the coherent transfer succeeds. This
         * allocation and its flags have the same boot lifetime; no user-owned
         * surface can address the private program slots. */
        if (!nv_compute_upload(ch, sh_off, sass, sass_len)) {
            kerr("nv-chan", "compute[%s]: resident program upload failed", label);
            return false;
        }
        resident_program_loaded[resident] = true;
    }
    sentinel = next_completion_signal(ch, sentinel);

    /* 1. Build the changing constant bank and QMD into sysmem staging. Legacy
     * shaders also stage their code here; resident programs never touch the
     * method ring above 64KiB. All embedded addresses still point at VRAM. */
    for (u32 i = 0; i < layout.bytes; i++) base[data_off + i] = 0;
    if (resident == NV_RESIDENT_PROGRAMS)
        for (u32 i = 0; i < sass_len; i++) base[sh_off + i] = sass[i];
    /* CUDA's Blackwell ABI constant bank is not all zeros even for an empty
     * kernel.  ptxas emits `LDC R1, c[0x0][0x37c]` as the first instruction;
     * tinygrad's working direct driver fills the two address windows and the
     * initial stack-pointer value below.  Leaving 0x37c zero was our largest
     * remaining divergence from the ptxas launch path. */
    *(volatile u32 *)(base + cb_off + 0x2f0) = 0u;          /* shared window lo */
    *(volatile u32 *)(base + cb_off + 0x2f4) = 1u;          /* shared window hi */
    *(volatile u32 *)(base + cb_off + 0x2f8) = 0xff000000u; /* local window lo  */
    *(volatile u32 *)(base + cb_off + 0x2fc) = 0u;          /* local window hi  */
    *(volatile u32 *)(base + cb_off + 0x37c) = 0x00fffdc0u; /* initial R1       */
    /* blockDim (ntid) at the CUDA sm_120 ABI offsets the ptxas kernels read for
     * blockIdx*blockDim+threadIdx: c[0x0][0x360]=blockDim.x, 0x364=y, 0x368=z
     * (verified in the tri_raster SASS: IMAD x = c[0x360]*CTAID.X + TID.X).  Left
     * unfilled it reads 0, collapsing a multi-block grid onto one tile at the
     * origin - which is why the rasterizer ran but drew nothing. */
    if (block) {
        *(volatile u32 *)(base + cb_off + 0x360) = block[0];
        *(volatile u32 *)(base + cb_off + 0x364) = block[1];
        *(volatile u32 *)(base + cb_off + 0x368) = block[2];
    }
    /* CUDA sm_120 ABI: kernel arguments arrive in constant bank 0 at c[0x0][0x380]
     * in declaration order.  A pointer arg is 8 bytes (two words); scalars are one
     * word each.  The global-store descriptor at c[0x0][0x358] stays zero - the
     * writeval kernel proved stores work without it. */
    if (args && args_words) {
        for (u32 i = 0; i < args_words; i++)
            *(volatile u32 *)(base + cb_off + 0x380 + i * 4) = args[i];
    }

    static u32 qmd[NV_QMD_WORDS];
    nv_compute_launch_t l = { 0 };
    l.program_addr = sh_va;
    l.grid[0]  = grid  ? grid[0]  : 1; l.grid[1]  = grid  ? grid[1]  : 1; l.grid[2]  = grid  ? grid[2]  : 1;
    l.block[0] = block ? block[0] : 1; l.block[1] = block ? block[1] : 1; l.block[2] = block ? block[2] : 1;
    l.reg_count = reg_count; l.shared_bytes = 0;
    /* These shaders come from ptxas, not Mesa NAK.  Match the working
     * Blackwell CUDA path: SM version 0xa04 encodes as QMD SASS_VERSION 0xa4,
     * VIA_HEADER_INDEX sampler mode, one ABI barrier, group 0x3f, and an
     * explicit program-prefetch range.  NAK's zeros are valid for NAK code but
     * are not the right reference for a CUDA cubin. */
    l.program_size = sass_len;
    l.sass_version = 0xa4;
    l.sampler_index = 1;
    l.barrier_count = 1;
    l.qmd_group_id = 0x3f;
    nv_qmd_build_compute(qmd, &l);
    nv_qmd_set_constant_buffer(qmd, 0, cb_va, 0x400);
    if (sass == gl_raster_sass) {
        /* ptxas emits switch branch tables in bank 2. The QMD is 0x180
         * bytes; its reserved 0x200..0x400 gap fits this immutable table,
         * inside the same coherent upload and below the program at +0x400. */
        _Static_assert(NV_QMD_WORDS * 4u <= 0x200u, "QMD/bank2 separation");
        _Static_assert(sizeof gl_raster_sass_const2 <= 0x200u, "bank2 slot");
        for (u32 i = 0; i < sizeof gl_raster_sass_const2; i++)
            base[qmd_off + 0x200u + i] = gl_raster_sass_const2[i];
        nv_qmd_set_constant_buffer(qmd, 2, qmd_va + 0x200u, sizeof gl_raster_sass_const2);
    }
    if (sass == gui_raster_sass) {
        /* The line-capable widget kernel now has a compiler-owned bank-2
         * branch table. Its immutable program residency does not eliminate
         * this QMD binding: table bytes travel with each changing launch. */
        _Static_assert(sizeof gui_raster_sass_const2 <= 0x200u, "widget bank2 slot");
        for (u32 i = 0; i < sizeof gui_raster_sass_const2; i++)
            base[qmd_off + 0x200u + i] = gui_raster_sass_const2[i];
        nv_qmd_set_constant_buffer(qmd, 2, qmd_va + 0x200u, sizeof gui_raster_sass_const2);
    }
    if (sass == shader_raster_sass) {
        _Static_assert(sizeof shader_raster_sass_const2 <= 0x200u, "programmable bank2 slot");
        for (u32 i = 0; i < sizeof shader_raster_sass_const2; i++)
            base[qmd_off + 0x200u + i] = shader_raster_sass_const2[i];
        nv_qmd_set_constant_buffer(qmd, 2, qmd_va + 0x200u, sizeof shader_raster_sass_const2);
    }
    if (sass == shader_raster_fast_sass) {
        _Static_assert(sizeof shader_raster_fast_sass_const2 <= 0x200u, "register fragment bank2 slot");
        for (u32 i = 0; i < sizeof shader_raster_fast_sass_const2; i++)
            base[qmd_off + 0x200u + i] = shader_raster_fast_sass_const2[i];
        nv_qmd_set_constant_buffer(qmd, 2, qmd_va + 0x200u, sizeof shader_raster_fast_sass_const2);
    }
    if (sass == shader_setup_sass) {
        _Static_assert(sizeof shader_setup_sass_const2 <= 0x200u, "setup bank2 slot");
        for (u32 i = 0; i < sizeof shader_setup_sass_const2; i++)
            base[qmd_off + 0x200u + i] = shader_setup_sass_const2[i];
        nv_qmd_set_constant_buffer(qmd, 2, qmd_va + 0x200u, sizeof shader_setup_sass_const2);
    }
    for (u32 i = 0; i < NV_QMD_WORDS; i++)
        *(volatile u32 *)(base + qmd_off + i * 4) = qmd[i];

    /* 2. Publish QMD/constant-bank/code through a coherent COPY transfer before
     * PCAS. Boot uses RM MemUtils; runtime reuses the committed private mapping.
     * Never replace either path with PRAMIN plus a client-forbidden FB_FLUSH. */
    if (!inline_launch &&
        !nv_compute_upload(ch, data_off, (const void *)(base + data_off), layout.bytes)) {
        kerr("nv-chan", "compute[%s]: coherent QMD/program upload failed", label);
        return false;
    }
    ch->sem[0] = 0;
    __asm__ volatile("mfence" ::: "memory");

    u32 start = ch->pb_at;
    pb_method(ch, SUBCH_COMPUTE, CEC0_SET_OBJECT, 1);  pb_data(ch, BLACKWELL_COMPUTE_B);
    if(inline_launch && !nv_compute_inline_upload(ch,data_off,base+data_off,layout.bytes))
        return false;
    /* Surface shaders overwrite overlapping program regions at 8400/C400.
     * QMD INVALIDATE_SHADER_DATA_CACHE and INVALIDATE_SKED_CACHES do not
     * invalidate instructions. The previous launch has retired before this
     * upload; now invalidate code/data/constants before the new PCAS launch.
     * Preserve the boot-only legacy packet sequence before any surface shader
     * has run. After that, even a legacy shader may replace surface code and
     * must invalidate stale instructions before its PCAS launch. */
    static bool surface_shader_loaded;
    if (sass == gui_raster_sass || sass == gui_present_sass || sass == gl_raster_sass ||
        sass == shader_vm_sass || sass == shader_raster_sass || sass == shader_raster_fast_sass ||
        sass == shader_setup_sass || sass == shader_setup_compact_sass || sass == shader_setup_compact_small_sass)
        surface_shader_loaded = true;
    if (surface_shader_loaded) {
        pb_method(ch, SUBCH_COMPUTE, CEC0_INVALIDATE_SHADER_CACHES, 1);
        pb_data(ch, CEC0_INVALIDATE_SHADER_CODE_DATA_CONST);
    }
    /* Per-context init before the launch (NVK values for Hopper+/Blackwell): shared
     * window 4GB-aligned (1<<32), local window 0xff<<24.  Re-emitting per dispatch
     * is idempotent.  Without it the async grid faults and never signals. */
    pb_method(ch, SUBCH_COMPUTE, CEC0_INVALIDATE_SKED_CACHES, 1); pb_data(ch, 0);
    pb_method(ch, SUBCH_COMPUTE, CEC0_SET_SHADER_SHARED_MEMORY_WINDOW_A, 2);
        pb_data(ch, 1u);            /* (1ULL<<32) >> 32              */
        pb_data(ch, 0u);            /* (1ULL<<32) & 0xffffffff       */
    pb_method(ch, SUBCH_COMPUTE, CEC0_SET_SHADER_LOCAL_MEMORY_WINDOW_A, 2);
        pb_data(ch, 0u);            /* (0xffULL<<24) >> 32           */
        pb_data(ch, 0xff000000u);   /* (0xffULL<<24) & 0xffffffff    */
    pb_method(ch, SUBCH_COMPUTE, CEC0_SEND_PCAS_A, 1); pb_data(ch, (u32)(qmd_va >> 8));
    pb_method(ch, SUBCH_COMPUTE, CEC0_SEND_PCAS2_B, 1); pb_data(ch, CEC0_PCAS2_ACTION_ICS);
    pb_method(ch, SUBCH_COMPUTE, CEC0_WAIT_FOR_IDLE, 1); pb_data(ch, 0);
    if (pair && pair->recorded==0u) {
        /* Compute producer -> compute consumer: match Mesa's shader-write
         * dependency flush (DATA bit 4, FLUSH_DATA bit 2; implicit WFI).
         * A CPU wait is gone, not the memory dependency. */
        pb_method(ch,SUBCH_COMPUTE,CEC0_INVALIDATE_SHADER_CACHES,1);
        pb_data(ch,0x14u);
    }
    pb_method(ch, SUBCH_COMPUTE, CEC0_REPORT_SEM_ADDR_LO, 2);
        pb_data(ch, (u32)VA_SEM); pb_data(ch, (u32)(VA_SEM >> 32));
    pb_method(ch, SUBCH_COMPUTE, CEC0_REPORT_SEM_PAY_LO, 1); pb_data(ch, sentinel);
    pb_method(ch, SUBCH_COMPUTE, CEC0_REPORT_SEM_EXECUTE, 1);
        pb_data(ch, CEC0_REPORT_SEM_RELEASE_1W |
                    ((!pair || pair->recorded==1u)?
                     nv_completion_awaken(ch, COMPUTE_COMPLETION_AWAKEN):0u));

    if (pair) {
        /* Retain WFI and the flushing release BETWEEN grids. Compaction reads
         * setup output only after this ordering point; its final cookie is
         * the only one the CPU waits for. This return means recorded, not run. */
        pair->recorded++;
        pair->signal=sentinel;
        return true;
    }

    /* Desktop batches are frequent: serial logging each fence dominates the
     * rendering time. Preserve first-launch evidence and every error. */
    static u32 surface_launch_logs;
    bool report = strncmp(label,"surface/",8) != 0 || surface_launch_logs++ < 4u;
    if (report) kinfo("nv-chan", "compute[%s]: QMD@%#llx prog@%#llx cb@%#llx reg=%u - dispatching",
          label, (unsigned long long)qmd_va, (unsigned long long)sh_va,
          (unsigned long long)cb_va, reg_count);
    bool ok = submit_and_wait(ch, start, sentinel);
    if (report || !ok) kinfo("nv-chan", "compute[%s]: %s (sem read %#x)", label,
          ok ? "grid ran to idle on the real card" : "no completion semaphore",
          ch->sem[0]);
    if (!ok) {
        /* Read GR's own exception registers directly over BAR0.  The RC journal
         * (GSP-side) named a GR_EXCEPTION but not the sub-unit; 0x407020 is the
         * SKED status, which decodes (gk104_sked_error, nouveau gf100.c) to the
         * exact QMD field SKED rejected.  0x400108 is the TRAP sub-source bitmap
         * (bit8=SKED).  If these read 0xbadfxxxx they are PLM-blocked under GSP
         * and the answer must come from the GSP journal instead. */
        nv_card_t *card = ch->card;
        /* CWD_FS (0x405b00): bits 7:0 = #GPCs, 15:8 = #TPCs the Compute Work
         * Distributor will dispatch to.  ZERO here = the running context has no
         * compute resources -> grid can never reach an SM (open-gpu-doc ga100
         * pri_cwd.ref.txt).  CWD_SM_ID[0] (0x405ba0) = SM routing table entry 0.
         * SKED_ACTIVITY (0x407054) shows where a grid is parked (0x407020 was the
         * SKED *error* status, which reads 0 on a hang and told us nothing). */
        kwarn("nv-chan", "GR@fault: CWD_FS(405b00)=%#x CWD_SM_ID0(405ba0)=%#x "
              "SKED_ACT(407054)=%#x SKED_ESR(407020)=%#x INTR(400100)=%#x "
              "TRAP(400108)=%#x TRAPADDR(400704)=%#x",
              nv_rd32(card, 0x405b00u), nv_rd32(card, 0x405ba0u),
              nv_rd32(card, 0x407054u), nv_rd32(card, 0x407020u),
              nv_rd32(card, 0x400100u), nv_rd32(card, 0x400108u),
              nv_rd32(card, 0x400704u));
    }
    return ok;
}

static bool nv_compute_dispatch_one(nv_channel_t *ch,
                                    const unsigned char *sass,u32 sass_len,
                                    u32 reg_count,u32 data_off,
                                    const u32 *args,u32 args_words,
                                    const u32 *grid,const u32 *block,
                                    u32 sentinel,const char *label) {
    return nv_compute_dispatch_record(ch,sass,sass_len,reg_count,data_off,
        args,args_words,grid,block,sentinel,label,NULL);
}

/* Setup and compaction are consecutive GPU producers with no CPU consumer
 * between them. Keep their WFI/flushing release barriers, but publish both in
 * one GPFIFO entry and wait once. The existing metadata readback and ALL
 * semantic gates still follow completion before any framebuffer writes.
 * Cold resident-code uploads may use COPY before publication, never after it.
 * A failure while recording discards the UNPUBLISHED methods, not live work. */
static bool nv_compute_geometry_pair(nv_channel_t *ch,const u32 *setup_args,
                                     const u32 *compact_args,u32 triangles) {
    if (!ch || !ch->open || ch->submit_failed || !g_render_transaction ||
        !setup_args || !compact_args || !triangles || triangles>KSHS_MAX_TRIANGLES)
        return false;
    u32 grid[3]={(triangles+63u)/64u,1u,1u},block[3]={64u,1u,1u};
    bool small=triangles<=32u;
    const u8 *compact=small?shader_setup_compact_small_sass:shader_setup_compact_sass;
    u32 compact_size=small?sizeof shader_setup_compact_small_sass:sizeof shader_setup_compact_sass;
    u32 compact_regs=small?shader_setup_compact_small_sass_reg_count:shader_setup_compact_sass_reg_count;
    if (!nv_surface_transfer_ce_ready()) {
        /* Boot/legacy publication still requires a coherent upload per grid. */
        return nv_compute_dispatch_one(ch,shader_setup_sass,sizeof shader_setup_sass,
            shader_setup_sass_reg_count,0x400u,setup_args,24u,grid,block,0x53550000u,"surface/geometry-setup") &&
            nv_compute_dispatch_one(ch,compact,compact_size,compact_regs,0xc00u,
                compact_args,21u,grid,block,0x53430000u,"surface/geometry-compact");
    }
    const u32 capacity=2u*(0x800u+160u);
    if (!ensure_compute_vram(ch) || !pb_reserve(ch,capacity)) return false;
    nv_compute_pair_t pair={ch->pb_at,ch->pb_at+capacity,0u,0u};
    if (!nv_compute_dispatch_record(ch,shader_setup_sass,sizeof shader_setup_sass,
            shader_setup_sass_reg_count,0x400u,setup_args,24u,grid,block,
            0x53550000u,"surface/geometry-setup",&pair) ||
        !nv_compute_dispatch_record(ch,compact,compact_size,compact_regs,0xc00u,
            compact_args,21u,grid,block,0x53430000u,"surface/geometry-compact",&pair)) {
        ch->pb_at=pair.start;
        return false;
    }
    bool ok=submit_and_wait(ch,pair.start,pair.signal);
    if (!ok) kerr("nv-chan","compute[geometry-pair]: no final completion; no metadata consumption or replay");
    static bool reported;
    if (ok && !reported) {
        kinfo("nv-chan","geometry setup+compact retired: two GPU grids, one submission, one CPU wait; semantic validation follows");
        reported=true;
    }
    return ok;
}

/* Off-screen surfaces use stable VA slots and generation-tagged handles. Reuse
 * of a slot does not allocate another set of page tables. The pool bounds
 * aggregate resource count, while each allocation is sized to its dimensions.
 * GPUOP_SURFACE is the snapshotted/serialized public frontend (nv_surface.h). */
#define NV_SURFACE_SLOTS 32u
#define NV_SURFACE_MAX_BYTES 0x10000000ull
#define VA_SURFACE_BASE 0x0030000000000ull
#define H_SURFACE_BASE 0x00500000u
typedef enum { NS_FREE, NS_BUILDING, NS_READY, NS_QUARANTINED } nv_surface_state_t;
typedef struct {
    u64 owner, generation, fb, va, bytes;
    u32 memory, width, height, pitch;
    nv_surface_state_t state;
    bool allocated, mapping_attempted, copy_mapping_attempted;
} nv_surface_t;
static nv_surface_t g_surfaces[NV_SURFACE_SLOTS];

static bool nv_surface_dimensions(u32 width, u32 height, nv_surface_info_t *out) {
    if (!out || !width || !height || width > 32768u || height > 32768u)
        return false;
    u64 pitch = ((u64)width * 4u + 255u) & ~255ull;
    u64 bytes = (pitch * height + 0xffffu) & ~0xffffull;
    if (bytes > NV_SURFACE_MAX_BYTES) return false;
    *out = (nv_surface_info_t){width, height, (u32)pitch, bytes};
    return true;
}

bool nv_surface_query_dimensions(u32 width, u32 height, nv_surface_info_t *out) {
    return nv_surface_dimensions(width,height,out);
}

static nv_surface_t *nv_surface_lookup(u64 owner, u64 handle) {
    u32 slot = (u32)(handle & 255u);
    if (!owner || !slot || slot > NV_SURFACE_SLOTS) return NULL;
    nv_surface_t *s = &g_surfaces[slot - 1u];
    if (s->state != NS_READY || s->owner != owner ||
        s->generation != (handle >> 8)) return NULL;
    return s;
}

/* No dereference or source upload happens before all records pass this gate. */
static bool nv_surface_commands_valid(const kg2d_command_t *commands, u32 count,
                                      const nv_surface_t *source) {
    if (!commands || !count || count > KG2D_MAX_COMMANDS) return false;
    for (u32 i = 0; i < count; i++) {
        const kg2d_command_t *c = &commands[i];
        bool scaled = c->op == KG2D_IMAGE_SCALED || c->op == KG2D_ARGB_BILINEAR;
        if (c->width <= 0 || c->height <= 0 || c->op > KG2D_GLYPH ||
            c->opacity > 255u ||
            (!scaled && (c->reserved[0] || c->reserved[1])) ||
            (c->op == KG2D_ARGB_BILINEAR && c->colour1 > 1u) ||
            c->reserved[2] || c->reserved[3]) return false;
        if (c->op == KG2D_SHADOW &&
            (!c->source_x || c->source_x>4096u || c->source_y>4096u ||
             (u32)c->width<=2u*c->source_x || (u32)c->height<=2u*c->source_x+2u ||
             c->source_stride || c->source_offset)) return false;
        if(c->op==KG2D_ROUNDED &&
           (c->source_x>4096u || c->source_y>5u || c->source_x>(u32)c->width/2u ||
            c->source_x>(u32)c->height/(c->source_y==2u || c->source_y==3u?1u:2u) ||
             c->source_stride || c->source_offset))return false;
        if((c->op==KG2D_LINE || c->op==KG2D_LINE_AA) && (c->colour1>3u || c->source_x || c->source_y ||
                               c->source_stride || c->source_offset))return false;
        if(c->op==KG2D_LINE_AA) {
            int major=(c->colour1&1u?c->height:c->width)-1;
            int minor=(c->colour1&1u?c->width:c->height)-2;
            if(minor<=0 || major<minor)return false;
        }
        if(c->op==KG2D_GLYPH) {
            if(!source || !c->source_x || !c->source_y || c->colour1>1u)return false;
            u64 row_bytes=c->colour1?((u64)c->source_x+7u)/8u:c->source_x;
            u64 end=(u64)(c->source_y-1u)*c->source_stride+row_bytes;
            if(row_bytes>c->source_stride || c->source_offset>source->bytes ||
               end>source->bytes-c->source_offset)return false;
        }
        if (c->op == KG2D_IMAGE || scaled || c->op == KG2D_MASK_A8) {
            if (!source) return false;
            u64 bpp = c->op == KG2D_MASK_A8 ? 1u : 4u;
            u32 sw = scaled ? c->reserved[0] : (u32)c->width;
            u32 sh = scaled ? c->reserved[1] : (u32)c->height;
            if (!sw || !sh) return false;
            u64 right = (u64)c->source_x + sw;
            u64 bottom = (u64)c->source_y + sh;
            if (right > 0x100000000ull || bottom > 0x100000000ull ||
                right * bpp > c->source_stride) return false;
            /* u32 stride * at most UINT32_MAX rows cannot overflow u64;
             * subtract remaining allocation bytes before adding the row span. */
            u64 row = (bottom - 1u) * c->source_stride;
            if (c->source_offset > source->bytes ||
                row > source->bytes - c->source_offset ||
                right * bpp > source->bytes - c->source_offset - row)
                return false;
        }
    }
    return true;
}

#define NV_GUI_DISPATCH_OFF 0x8000u
_Static_assert(KG2D_MAX_COMMANDS*sizeof(kg2d_command_t)<=NV_GUI_DISPATCH_OFF,
               "widget commands overlap shader staging");
_Static_assert(((NV_GUI_DISPATCH_OFF+0x400u+sizeof gui_raster_sass+0x3ffu)&~0x3ffu)+0x400u<=0x10000u,
               "widget QMD/program/constants exceed staging");
static bool nv_surface_dispatch(nv_surface_t *dst, const nv_surface_t *src,
                                const kg2d_command_t *commands, u32 count,
                                u32 width, u32 height, u32 pitch,
                                u32 x, u32 y, u32 clip_w, u32 clip_h) {
    nv_channel_t *ch = &channels[CH_GFX];
    if (!ch->open || ch->submit_failed || !ensure_compute_vram(ch)) return false;
    /* Commands occupy 0..0x4000; QMD/program/constants start at 0x8000.
     * The enlarged rounded-shape shader no longer fits at 0xc000. The command
     * snapshot is kernel-owned and source/destination remain pinned to fence. */
    if (!nv_compute_upload(ch, 0, commands, count * sizeof(*commands))) return false;
    u64 source_va = src ? src->va : 0, source_bytes = src ? src->bytes : 0;
    u32 args[16] = {
        (u32)dst->va, (u32)(dst->va >> 32),
        (u32)source_va, (u32)(source_va >> 32),
        (u32)VA_COMPUTE, (u32)(VA_COMPUTE >> 32),
        (u32)source_bytes, (u32)(source_bytes >> 32),
        width, height, pitch / 4u, count, x, y, clip_w, clip_h
    };
    u32 grid[3] = {(clip_w + 15u) / 16u, (clip_h + 15u) / 16u, 1u};
    u32 block[3] = {16u, 16u, 1u};
    return nv_compute_dispatch_one(ch, gui_raster_sass, sizeof gui_raster_sass,
                                   gui_raster_sass_reg_count, NV_GUI_DISPATCH_OFF,
                                   args, 16u, grid, block, 0x47320000u, "surface/2d");
}

static bool nv_surface_dispose(nv_surface_t *s) {
    nv_channel_t *ch = &channels[CH_GFX];
    nv_channel_t *copy = &channels[CH_COPY];
    /* A failed submission may still access ANY surface from the last batch.
     * Keep all allocations mapped and owned until channel recovery/reset. */
    if (ch->submit_failed || copy->submit_failed) {
        s->state = NS_QUARANTINED; return false;
    }
    if (s->mapping_attempted &&
        (!nv_vmm_unmap(&ch->vmm, s->va, s->bytes) ||
         !nv_vmm_commit(ch->card, ch->rm, &ch->vmm, h_vaspace(CH_GFX),
                         "surface unmap"))) {
        s->state = NS_QUARANTINED;
        return false;
    }
    s->mapping_attempted = false;
    if (s->copy_mapping_attempted &&
        (!nv_vmm_unmap(&copy->vmm, s->va, s->bytes) ||
         !nv_vmm_commit(copy->card, copy->rm, &copy->vmm, h_vaspace(CH_COPY),
                          "surface copy unmap"))) {
        s->state = NS_QUARANTINED;
        return false;
    }
    s->copy_mapping_attempted = false;
    if (s->allocated && !nv_rm_free(ch->card, ch->rm, RM_DEVICE, s->memory)) {
        s->state = NS_QUARANTINED;
        return false;
    }
    u64 generation = s->generation;
    memset(s, 0, sizeof *s);
    s->generation = generation;
    return true;
}

u64 nv_surface_create_with_pressure(u64 owner, u32 width, u32 height,
                                    bool *pool_exhausted) {
    if (pool_exhausted) *pool_exhausted = false;
    nv_channel_t *ch = &channels[CH_GFX];
    nv_channel_t *copy = &channels[CH_COPY];
    nv_surface_info_t info;
    if (!owner || !ch->open || ch->submit_failed ||
        !copy->open || copy->submit_failed ||
        !nv_surface_dimensions(width, height, &info)) return 0;
    nv_surface_t *s = NULL;
    u32 slot;
    for (slot = 0; slot < NV_SURFACE_SLOTS; slot++) {
        if (g_surfaces[slot].state == NS_FREE &&
            g_surfaces[slot].generation < 0x00ffffffffffffffull) {
            s = &g_surfaces[slot]; break;
        }
    }
    if (!s) {
        if (pool_exhausted) *pool_exhausted = true;
        return 0;
    }
    s->state = NS_BUILDING;
    s->owner = owner; s->generation++;
    s->width = width; s->height = height; s->pitch = info.pitch; s->bytes = info.bytes;
    s->va = VA_SURFACE_BASE + slot * NV_SURFACE_MAX_BYTES;
    s->memory = H_SURFACE_BASE + slot;
    const char *stage = "RM allocation";
    /* Allocate here rather than through nv_vram_alloc: partial construction
     * must remember the live RM object even if its physical query fails. */
    mem_alloc_params_t mp = {0};
    mp.owner = 0x4b455354u; mp.type = NVOS32_TYPE_IMAGE;
    mp.attr = NVOS32_ATTR_VIDMEM_CONTIGUOUS;
    mp.attr2 = NVOS32_ATTR2_GPU_CACHEABLE_NO << 2;
    mp.flags = NVOS32_ALLOC_FLAGS_NO_SCANOUT | NVOS32_ALLOC_FLAGS_ALIGN_FORCE;
    mp.size = s->bytes; mp.alignment = 0x10000u;
    if (!nv_rm_alloc(ch->card, ch->rm, RM_DEVICE, s->memory,
                      NV01_MEMORY_LOCAL_USER, &mp, sizeof mp)) {
        kerr("nv-chan", "surface RM alloc status=%#x", nv_last_alloc_status);
        goto fail;
    }
    s->allocated = true;
    stage = "physical attributes";
    phys_attr_params_t pa = {0};
    u32 got = 0;
    if (!nv_rm_control(ch->card, ch->rm, s->memory,
                        NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR,
                        &pa, sizeof pa, &pa, sizeof pa, &got) || got < sizeof pa ||
        pa.mem_aperture != NV0041_APERTURE_VIDMEM || (pa.mem_offset & 0xffffu))
    {
        kerr("nv-chan", "surface phys query status=%#x got=%u aperture=%u FB=%#llx",
              nv_last_control_status, got, pa.mem_aperture, (unsigned long long)pa.mem_offset);
        goto fail;
    }
    /* GET_SURFACE_PHYS_ATTR returns a PTE kind, not NVOS32_ATTR_FORMAT.
     * GB202 dev_mmu.h defines uncompressed GENERIC_MEMORY as 6; RM returns
     * this for our linear image allocation. Preserve that kind in BOTH
     * VASpaces, rather than rejecting it or silently aliasing it as kind 0.
     * Compressed/depth/invalid kinds require contracts not implemented here. */
    if ((pa.mem_format != 0u && pa.mem_format != 6u) ||
        pa.contig_segment_size < s->bytes) {
        kerr("nv-chan", "surface allocation incompatible: kind=%#x contiguous=%llu requested=%llu",
             pa.mem_format, (unsigned long long)pa.contig_segment_size,
             (unsigned long long)s->bytes);
        goto fail;
    }
    s->fb = pa.mem_offset;
    stage = "GR map/commit";
    s->mapping_attempted = true;
    if (!nv_vmm_map_kind(&ch->vmm, s->va, s->fb, s->bytes,
                          true, false, false, pa.mem_format) ||
        !nv_vmm_commit(ch->card, ch->rm, &ch->vmm, h_vaspace(CH_GFX), "surface map"))
        goto fail;
    /* Bulk initialization is CAB5's constant-fill job, not widget painting.
     * Map the same RM allocation into COPY and GR; clear every byte including
     * padding with the proven GPU remap-constant path. Its flush/release fence
     * must retire before NS_READY permits any shader access. No CPU pixel
     * buffer or CPU clear/upload fallback is involved. */
    s->copy_mapping_attempted = true;
    stage = "COPY map/commit";
    if (!nv_vmm_map_kind(&copy->vmm, s->va, s->fb, s->bytes,
                          true, false, false, pa.mem_format) ||
        !nv_vmm_commit(copy->card, copy->rm, &copy->vmm,
                         h_vaspace(CH_COPY), "surface copy map")) goto fail;
    stage = "COPY zero-fill fence";
    if (!ce_fill32(copy, s->va, 16384u, 4096u,
                    (u32)(s->bytes / 16384u), 0u, 0x53430000u)) goto fail;
    static u32 allocation_logs;
    if (allocation_logs++ < 4u)
        kinfo("nv-chan", "surface ready: %ux%u pitch=%u bytes=%llu VA=%#llx FB=%#llx; CAB5 zero-fill fenced",
              width, height, s->pitch, (unsigned long long)s->bytes,
              (unsigned long long)s->va, (unsigned long long)s->fb);
    s->state = NS_READY;
    return (s->generation << 8) | (slot + 1u);
fail:
    kerr("nv-chan", "surface CREATE failed stage=%s owner=%llu slot=%u handle=%#x %ux%u pitch=%u bytes=%llu VA=%#llx FB=%#llx GR-quarantined=%u COPY-quarantined=%u",
          stage, (unsigned long long)owner, slot, s->memory, width, height, s->pitch,
          (unsigned long long)s->bytes, (unsigned long long)s->va,
          (unsigned long long)s->fb, ch->submit_failed, copy->submit_failed);
    if (ch->submit_failed) nv_fault_shadow_path(ch, s->va);
    if (copy->submit_failed) nv_fault_shadow_path(copy, s->va);
    (void)nv_surface_dispose(s);
    return 0;
}

u64 nv_surface_create(u64 owner, u32 width, u32 height) {
    return nv_surface_create_with_pressure(owner, width, height, NULL);
}

bool nv_surface_destroy(u64 owner, u64 handle) {
    nv_surface_t *s = nv_surface_lookup(owner, handle);
    return s && nv_surface_dispose(s);
}

bool nv_surface_release_owner(u64 owner) {
    if (!owner) return false;
    bool ok = true;
    for (u32 i = 0; i < NV_SURFACE_SLOTS; i++) {
        nv_surface_t *s = &g_surfaces[i];
        if (s->state != NS_FREE && s->owner == owner && !nv_surface_dispose(s))
            ok = false;
    }
    return ok;
}

bool nv_surface_info(u64 owner, u64 handle, nv_surface_info_t *out) {
    nv_surface_t *s = nv_surface_lookup(owner, handle);
    if (!s || !out) return false;
    *out = (nv_surface_info_t){s->width, s->height, s->pitch, s->bytes};
    return true;
}

static bool nv_surface_transfer_ce_ready(void) {
    nv_channel_t *copy = &channels[CH_COPY];
    return copy->open && copy->rm && copy->rm->host_api && g_upload_stage_ready &&
           g_upload_stage && g_render_pin && g_render_pin == proc_current() &&
           sched_device_wait_allowed();
}

/* Runtime surfaces already have committed COPY and GR mappings. Reuse the
 * persistent, kernel-owned sysmem staging allocation instead of asking RM's
 * mem_utils.c to allocate/map/free a temporary staging surface on every
 * attribute/status transfer. No user pointer is ever a DMA destination.
 * The caller holds the pinned render transaction across every retired chunk. */
static bool nv_runtime_copy_transfer(u64 gpu, void *data, u32 bytes, bool read) {
    nv_channel_t *copy = &channels[CH_COPY];
    if (copy->submit_failed || !data || ((gpu | bytes) & 3u) ||
        !nv_surface_transfer_ce_ready() || gpu+bytes < gpu) return false;
    u8 *cpu = data;
    for (u32 done = 0; done < bytes;) {
        u32 chunk = bytes - done;
        if (chunk > UPLOAD_STAGE_BYTES) chunk = UPLOAD_STAGE_BYTES;
        if (!read) memcpy((void *)g_upload_stage, cpu + done, chunk);
        /* Write back dirty old cache lines before DMA in either direction.
         * For readback, invalidate again only AFTER the release cookie so a
         * speculative CPU read cannot leave stale lines visible to memcpy. */
        cache_flush(g_upload_stage, chunk);
        u64 at = gpu + done;
        if (!ce_copy(copy, read ? at : VA_UPLOAD_STAGE,
                      read ? VA_UPLOAD_STAGE : at, chunk, chunk, chunk, 1u,
                      read ? 0x53520000u : 0x53570000u))
            return false; /* no replay or RM fallback after a failed submission */
        if (read) {
            cache_flush(g_upload_stage, chunk);
            memcpy(cpu + done, (const void *)g_upload_stage, chunk);
        }
        done += chunk;
    }
    return true;
}

bool nv_surface_transfer(u64 owner, u64 handle, u64 offset,
                         void *data, u32 bytes, bool read) {
    nv_surface_t *s = nv_surface_lookup(owner, handle);
    nv_channel_t *ch = &channels[CH_GFX];
    if (!s || ch->submit_failed || channels[CH_COPY].submit_failed ||
        !data || ((offset | bytes) & 3u) ||
        offset > s->bytes || bytes > s->bytes - offset) return false;
    if (nv_surface_transfer_ce_ready())
        return s->copy_mapping_attempted &&
               nv_runtime_copy_transfer(s->va+offset, data, bytes, read);
    /* Boot/unpinned callers retain the existing coherent RM path. This is a
     * pre-submission choice, never recovery from an unfenced COPY operation. */
    return read ? nv_vram_object_read(ch, s->memory, s->fb, offset, data, bytes)
                : nv_vram_object_write(ch, s->memory, s->fb, offset, data, bytes);
}

void nv_surface_reap_exited(void) {
    for (u32 i = 0; i < NV_SURFACE_SLOTS; i++) {
        nv_surface_t *s = &g_surfaces[i];
        if (s->owner && !proc_gpu_owner_alive(s->owner))
            nv_surface_release_owner(s->owner);
    }
}

bool nv_surface_draw(u64 owner, u64 destination, u64 source,
                     const kg2d_command_t *commands, u32 count,
                     u32 x, u32 y, u32 width, u32 height) {
    nv_surface_t *dst = nv_surface_lookup(owner, destination);
    nv_surface_t *src = source ? nv_surface_lookup(owner, source) : NULL;
    if (!dst || (source && !src) || (src && src == dst) ||
        !width || !height || x >= dst->width || y >= dst->height ||
        width > dst->width - x || height > dst->height - y ||
        !nv_surface_commands_valid(commands, count, src)) return false;
    return nv_surface_dispatch(dst, src, commands, count,
                                dst->width, dst->height, dst->pitch,
                                x, y, width, height);
}

/* Validate IEEE-754 bits without issuing floating-point instructions in the
 * kernel. The GPU consumes the float fields; the kernel only bounds/snapshots
 * them. memcpy avoids type-punning and unaligned-access assumptions. */
static u32 nv_float_bits(const float *value) {
    u32 bits; memcpy(&bits, value, sizeof bits); return bits;
}
static bool nv_surface_3d_commands_valid(const kg3d_command_t *commands, u32 count,
                                         bool depth, bool texture) {
    if (!commands || !count || count > KG3D_MAX_COMMANDS) return false;
    for (u32 i = 0; i < count; i++) {
        const kg3d_command_t *c = &commands[i];
        bool clear = !!(c->flags & (KG3D_CLEAR_COLOUR | KG3D_CLEAR_DEPTH));
        u32 primitive = c->flags & (KG3D_LINE | KG3D_POINT);
        if (primitive && (primitive == (KG3D_LINE | KG3D_POINT) ||
            (c->flags & (KG3D_TEXTURE | KG3D_CLEAR_COLOUR | KG3D_CLEAR_DEPTH)))) return false;
        if (c->width <= 0 || c->height <= 0 || (c->flags & ~KG3D_FLAGS) ||
            c->depth_func > KG3D_ALWAYS || c->alpha_func > KG3D_ALWAYS ||
            c->blend_src > KG3D_ONE_MINUS_DST_ALPHA || c->blend_dst > KG3D_ONE_MINUS_DST_ALPHA ||
            c->tex_env > KG3D_DECAL || c->wrap_s > KG3D_CLAMP || c->wrap_t > KG3D_CLAMP ||
            c->filter > KG3D_LINEAR || c->reserved[0] || c->reserved[1] || c->reserved[2] ||
            ((c->flags & (KG3D_DEPTH_TEST | KG3D_DEPTH_WRITE | KG3D_CLEAR_DEPTH)) && !depth) ||
            (!clear && (c->flags & KG3D_TEXTURE) && !texture)) return false;
        /* Every float consumed by the shader must be finite, including clear
         * values. Non-finite unused vertex slots are rejected too. */
        for (u32 j = 0; j < 3; j++) {
            const kg3d_vertex_t *v = &c->v[j];
            u32 xb = nv_float_bits(&v->x) & 0x7fffffffu;
            u32 yb = nv_float_bits(&v->y) & 0x7fffffffu;
            u32 wb = nv_float_bits(&v->inv_w);
            if (xb > 0x49800000u || yb > 0x49800000u ||
                (nv_float_bits(&v->z) & 0x7f800000u) == 0x7f800000u ||
                (wb & 0x7f800000u) == 0x7f800000u ||
                (!clear && ((wb & 0x80000000u) || !wb))) return false;
            for (u32 k = 0; k < 4; k++)
                if ((nv_float_bits(&v->rgba[k]) & 0x7f800000u) == 0x7f800000u) return false;
            for (u32 k = 0; k < 2; k++)
                if ((nv_float_bits(&v->uv[k]) & 0x7f800000u) == 0x7f800000u) return false;
        }
        if ((nv_float_bits(&c->depth_bias) & 0x7f800000u) == 0x7f800000u ||
            (nv_float_bits(&c->alpha_ref) & 0x7f800000u) == 0x7f800000u) return false;
    }
    return true;
}

bool nv_surface_draw3d(u64 owner, u64 destination, u64 depth, u64 texture,
                       const kg3d_command_t *commands, u32 count,
                       u32 x, u32 y, u32 width, u32 height) {
    nv_channel_t *ch = &channels[CH_GFX];
    nv_surface_t *dst = nv_surface_lookup(owner, destination);
    nv_surface_t *z = depth ? nv_surface_lookup(owner, depth) : NULL;
    nv_surface_t *tex = texture ? nv_surface_lookup(owner, texture) : NULL;
    if (!dst || (depth && !z) || (texture && !tex) || dst == z || dst == tex ||
        (z && z == tex) || !width || !height || x >= dst->width || y >= dst->height ||
        width > dst->width-x || height > dst->height-y ||
        (z && (z->width != dst->width || z->height != dst->height)) ||
        !nv_surface_3d_commands_valid(commands, count, z != NULL, tex != NULL) ||
        !ch->open || ch->submit_failed) return false;
    u32 work = 0;
    for (u32 i = 0; i < count; i++)
        work += (commands[i].flags & KG3D_LINE) ? KG3D_LINE_WORK : 1u;
    if ((u64)width * height * work > KG3D_MAX_WORK || !ensure_compute_vram(ch)) return false;
    /* 64 x 192-byte records occupy 0..0x3000. The resident application shader
     * is above 64KiB; changing QMD/constants occupy 0x8000..0x8800.
     * No overlap with commands or the method ring; all surfaces remain pinned
     * under the caller's render transaction until the shared launcher retires. */
    if (!nv_compute_upload(ch, 0, commands, count * sizeof(*commands))) return false;
    u64 zv = z ? z->va : 0, tv = tex ? tex->va : 0, tb = tex ? tex->bytes : 0;
    u32 args[22] = {
        (u32)dst->va, (u32)(dst->va >> 32),
        (u32)zv, (u32)(zv >> 32), (u32)tv, (u32)(tv >> 32),
        (u32)VA_COMPUTE, (u32)(VA_COMPUTE >> 32), (u32)tb, (u32)(tb >> 32),
        dst->width, dst->height, dst->pitch / 4u, z ? z->pitch / 4u : 0,
        tex ? tex->width : 0, tex ? tex->height : 0, tex ? tex->pitch / 4u : 0,
        count, x, y, width, height
    };
    u32 grid[3] = {(width+15u)/16u, (height+15u)/16u, 1u};
    u32 block[3] = {16u, 16u, 1u};
    return nv_compute_dispatch_one(ch, gl_raster_sass, sizeof gl_raster_sass,
                                    gl_raster_sass_reg_count, 0x8000u,
                                    args, 22u, grid, block, 0x47330000u, "surface/3d");
}

static bool nv_surface_shader_valid(const ksh_dispatch_t *job, u64 texture_bytes) {
    if (!job || !job->lanes || job->lanes > KSH_MAX_LANES ||
        !job->code_count || job->code_count > SH_MAX_INSTRUCTIONS ||
        !job->budget || job->budget > KSH_MAX_STEPS ||
        (u64)job->lanes * job->budget > KSH_MAX_WORK ||
        job->texture_count > KSH_MAX_TEXTURES) return false;
    for (u32 i = 0; i < 4; i++) if (job->reserved[i]) return false;
    for (u32 i = 0; i < job->code_count; i++) {
        const sh_instruction_t *in = &job->code[i];
        if (in->op > SH_DISCARD || in->dst >= SR_REGISTERS || (in->mask & ~15u) ||
            in->src[0] >= SR_REGISTERS || in->src[1] >= SR_REGISTERS ||
            in->src[2] >= SR_REGISTERS ||
            (in->op == SH_MATMUL && in->src[0] > SR_REGISTERS-4) ||
            ((in->op == SH_JMP || in->op == SH_JMPZ || in->op == SH_JMPNZ) &&
             (in->target < 0 || (u32)in->target > job->code_count))) return false;
    }
    for (u32 i = 0; i < job->texture_count; i++) {
        const ksh_texture_t *t = &job->textures[i];
        if (!t->width && !t->height) continue;
        if (!t->width || !t->height || t->width > 32768u || t->height > 32768u ||
            t->pitch < t->width || t->wrap_s > 1u || t->wrap_t > 1u || t->filter > 1u ||
            (t->offset & 3u) || t->offset > texture_bytes ||
            (u64)(t->height-1u)*t->pitch+t->width > (texture_bytes-t->offset)/4u)
            return false;
    }
    return true;
}

/* The validated VM's code and descriptors plus its pending status form one
 * preparation dependency. Submit all three on COPY with a single final fence,
 * rather than sleeping separately after each tiny upload. No consumer runs
 * before that fence, and the pinned transaction owns staging until retirement.
 * Destinations are private work storage and a resolved owner status surface. */
static bool nv_shader_prepare(nv_channel_t *ch, nv_surface_t *status,
                              const ksh_dispatch_t *job) {
    u32 code_bytes=job->code_count*sizeof(sh_instruction_t);
    u32 texture_bytes=job->texture_count*sizeof(ksh_texture_t);
    u32 status_bytes=job->lanes*sizeof(ksh_status_t);
    if (!g_render_transaction || !nv_surface_transfer_ce_ready() || !status ||
        status->state!=NS_READY || !status->copy_mapping_attempted ||
        !code_bytes || code_bytes>0x4000u || texture_bytes>0x2000u ||
        code_bytes+texture_bytes>UPLOAD_STAGE_BYTES || !status_bytes ||
        status_bytes>status->bytes || status_bytes/4u>32768u ||
        !nv_compute_copy_mapping(ch)) return false;
    nv_ce_prepare_t ops[3]={
        {.dst=status->va,.src_pitch=status_bytes,.dst_pitch=status_bytes,
         .line=status_bytes/4u,.lines=1u,.fill=true},
        {.src=VA_UPLOAD_STAGE,.dst=VA_COMPUTE,.src_pitch=code_bytes,
         .dst_pitch=code_bytes,.line=code_bytes,.lines=1u},
        {.src=VA_UPLOAD_STAGE+code_bytes,.dst=VA_COMPUTE+0xe000u,
         .src_pitch=texture_bytes,.dst_pitch=texture_bytes,
         .line=texture_bytes,.lines=1u}};
    memcpy((void *)g_upload_stage,job->code,code_bytes);
    if (texture_bytes)
        memcpy((void *)(g_upload_stage+code_bytes),job->textures,texture_bytes);
    cache_flush(g_upload_stage,code_bytes+texture_bytes);
    return ce_prepare_batch(&channels[CH_COPY],ops,texture_bytes?3u:2u);
}

bool nv_surface_shader(u64 owner, u64 registers, u64 status, u64 texture,
                       const ksh_dispatch_t *job) {
    nv_channel_t *ch = &channels[CH_GFX];
    nv_surface_t *r = nv_surface_lookup(owner, registers);
    nv_surface_t *s = nv_surface_lookup(owner, status);
    nv_surface_t *t = texture ? nv_surface_lookup(owner, texture) : NULL;
    if (!r || !s || (texture && !t) || r == s || r == t || s == t ||
        !ch->open || ch->submit_failed || !nv_surface_shader_valid(job,t ? t->bytes : 0) ||
        r->bytes < (u64)job->lanes*SR_REGISTERS*4u*sizeof(float) ||
        s->bytes < (u64)job->lanes*sizeof(ksh_status_t)) return false;
    /* [0,0x4000) bytecode; dynamic QMD/bank2/cbuf start at 0x4000.
     * Native code uses its separate immutable resident slot. Descriptors at
     * 0xe000 must remain beyond the dynamic upload. */
    enum { VM_QMD=0x4000, VM_TEXTURES=0xe000 };
    nv_dispatch_layout_t layout;
    if (!nv_compute_resident_layout(VM_QMD, 24u, &layout) ||
        layout.constant_off+0x400u > VM_TEXTURES ||
        job->code_count*sizeof(sh_instruction_t) > VM_QMD ||
        !ensure_compute_vram(ch)) return false;
    static const ksh_status_t pending[KSH_MAX_LANES] = {{0,0}};
    if (nv_surface_transfer_ce_ready()) {
        if (!nv_shader_prepare(ch,s,job)) return false;
    } else if (!nv_surface_transfer(owner, status, 0, (void *)pending,
                              job->lanes*sizeof(ksh_status_t), false) ||
        !nv_compute_upload(ch, 0, job->code, job->code_count*sizeof(sh_instruction_t)) ||
        (job->texture_count &&
         !nv_compute_upload(ch, VM_TEXTURES, job->textures,
                              job->texture_count*sizeof(ksh_texture_t)))) return false;
    u64 cv=VA_COMPUTE, tv=t ? t->va : 0, dv=VA_COMPUTE+VM_TEXTURES;
    u64 rf=r->bytes/sizeof(float), cb=job->code_count*sizeof(sh_instruction_t);
    u64 sb=job->lanes*sizeof(ksh_status_t), tb=t ? t->bytes : 0;
    u64 db=job->texture_count*sizeof(ksh_texture_t);
    u32 args[24] = {
        (u32)r->va, (u32)(r->va>>32), (u32)cv, (u32)(cv>>32),
        (u32)s->va, (u32)(s->va>>32), (u32)tv, (u32)(tv>>32), (u32)dv, (u32)(dv>>32),
        (u32)rf, (u32)(rf>>32), (u32)cb, (u32)(cb>>32), (u32)sb, (u32)(sb>>32),
        (u32)tb, (u32)(tb>>32), (u32)db, (u32)(db>>32),
        job->lanes, job->code_count, job->budget, job->texture_count
    };
    u32 grid[3]={(job->lanes+63u)/64u,1,1}, block[3]={64,1,1};
    /* The render transaction owns all handles until retirement. A timeout
     * quarantines CH_GFX and existing surface disposal retains its mappings. */
    return nv_compute_dispatch_one(ch,shader_vm_sass,sizeof shader_vm_sass,
                                    shader_vm_sass_reg_count,VM_QMD,args,24u,grid,block,
                                    0x53560000u,"surface/shader-vm");
}

/* Immutable CPU-snapshotted raster state only. This does NOT validate command
 * geometry/varyings, establish command provenance, or admit their work. Keep
 * it separate so a future GPU setup operation can reuse the state checks
 * without weakening the public CPU-command validation below. */
static bool nv_surface_shader_raster_state_valid(const kshr_job_t *job, u64 texture_bytes) {
    if (!job || !job->clip_w || !job->clip_h ||
        (u64)job->clip_w*job->clip_h > KSH_MAX_WORK ||
        !job->command_count || job->command_count > KG3D_MAX_COMMANDS ||
        !job->code_count || job->code_count > SH_MAX_INSTRUCTIONS ||
        !job->budget || job->budget > KSH_MAX_STEPS ||
        job->texture_count > KSH_MAX_TEXTURES || job->varying_count > SR_VARYING_N ||
        job->origin_lower_left > 1u || job->reserved[0] || job->reserved[1]) return false;
    for (u32 r = 0; r < SR_REGISTERS; r++) for (u32 k = 0; k < 4u; k++)
        if ((nv_float_bits(&job->seed[r][k]) & 0x7f800000u) == 0x7f800000u) return false;
    for (u32 i = 0; i < job->code_count; i++) {
        const sh_instruction_t *in = &job->code[i];
        if (in->op > SH_DISCARD || in->dst >= SR_REGISTERS || (in->mask & ~15u) ||
            in->src[0] >= SR_REGISTERS || in->src[1] >= SR_REGISTERS || in->src[2] >= SR_REGISTERS ||
            (in->op == SH_MATMUL && in->src[0] > SR_REGISTERS-4) ||
            ((in->op == SH_JMP || in->op == SH_JMPZ || in->op == SH_JMPNZ) &&
             (in->target < 0 || (u32)in->target > job->code_count))) return false;
    }
    for (u32 i = 0; i < job->texture_count; i++) {
        const ksh_texture_t *t = &job->textures[i];
        if (!t->width && !t->height) continue;
        if (!t->width || !t->height || t->width > 32768u || t->height > 32768u ||
            t->pitch < t->width || t->wrap_s > 1u || t->wrap_t > 1u || t->filter > 1u ||
            (t->offset & 3u) || t->offset > texture_bytes ||
            (u64)(t->height-1u)*t->pitch+t->width > (texture_bytes-t->offset)/4u) return false;
    }
    return true;
}

/* Public CPU-command gate: state checks alone are never sufficient. Every
 * supplied record, including unused varying lanes, remains validated before
 * any upload/dispatch, followed by conservative per-region work admission. */
static bool nv_surface_shader_raster_valid(const kshr_job_t *job, bool depth, u64 texture_bytes) {
    if (!nv_surface_shader_raster_state_valid(job,texture_bytes)) return false;
    for (u32 i = 0; i < job->command_count; i++) {
        const kshr_command_t *cmd = &job->commands[i];
        if ((cmd->raster.flags & (KG3D_TEXTURE|KG3D_CLEAR_COLOUR|KG3D_CLEAR_DEPTH)) ||
            !nv_surface_3d_commands_valid(&cmd->raster,1u,depth,false)) return false;
        for (u32 v = 0; v < 3u; v++) for (u32 r = 0; r < SR_VARYING_N; r++)
            for (u32 k = 0; k < 4u; k++)
                if ((nv_float_bits(&cmd->varying[v][r][k]) & 0x7f800000u) == 0x7f800000u) return false;
    }
    return kshr_region_cost(job->commands,job->command_count,job->budget,
                           job->clip_x,job->clip_y,job->clip_w,job->clip_h)<=KSH_MAX_WORK;
}

typedef struct {
    nv_surface_t *dst,*src;
    u64 offset,source_offset;
    const void *upload;
    u32 bytes,record_bytes,records;
} nv_surface_prepare_t;
static bool nv_surface_prepare_batch(const nv_surface_prepare_t *ops,u32 count);

/* Private prepared-packet launch. No public request can call this directly:
 * callers must either fully validate CPU commands or establish trusted setup
 * provenance, validate all metadata and admit every window before any draw.
 * Packet/header/status publication must already have retired. */
static bool nv_surface_shader_raster_dispatch_prepared(nv_surface_t *dst,
    nv_surface_t *z, nv_surface_t *t, nv_surface_t *scratch,
    nv_surface_t *status, const kshr_job_t *job) {
    nv_channel_t *ch = &channels[CH_GFX];
    u32 lanes = kshr_job_lanes(job);
    u64 rb = kshr_job_scratch_bytes(job), po = KSHR_JOB_PACKET_OFFSET(job);
    u64 sb = (u64)lanes*sizeof(ksh_status_t);
    if (!lanes || scratch->bytes < KSHR_JOB_ALLOCATION_BYTES(job) || status->bytes < sb)
        return false;
    u64 zv = z ? z->va : 0, tv = t ? t->va : 0, pv = scratch->va+po;
    u64 zb = z ? z->bytes : 0, tb = t ? t->bytes : 0;
    u32 args[24] = {
        (u32)dst->va,(u32)(dst->va>>32), (u32)zv,(u32)(zv>>32), (u32)tv,(u32)(tv>>32),
        (u32)scratch->va,(u32)(scratch->va>>32), (u32)status->va,(u32)(status->va>>32),
        (u32)pv,(u32)(pv>>32), (u32)dst->bytes,(u32)(dst->bytes>>32),
        (u32)zb,(u32)(zb>>32), (u32)tb,(u32)(tb>>32), (u32)rb,(u32)(rb>>32),
        (u32)sb,(u32)(sb>>32), sizeof(*job),0
    };
    u32 grid[3] = {(lanes+63u)/64u,1,1}, block[3] = {64,1,1};
    if (kshr_register_fast_eligible(job))
        return nv_compute_dispatch_one(ch,shader_raster_fast_sass,sizeof shader_raster_fast_sass,
                                    shader_raster_fast_sass_reg_count,0x400u,args,24u,grid,block,
                                    0x53520000u,"surface/shader-raster-register");
    return nv_compute_dispatch_one(ch,shader_raster_sass,sizeof shader_raster_sass,
                                    shader_raster_sass_reg_count,0x400u,args,24u,grid,block,
                                    0x53520000u,"surface/shader-raster");
}

/* Private CPU-snapshot dispatch tail. Caller holds the render transaction,
 * resolves all surfaces from this owner, rejects every alias, matches their
 * dimensions, and passes the complete nv_surface_shader_raster_valid gate.
 * scratch/status MUST be the surfaces named in submission. This helper still
 * uploads validated CPU commands; it is not a GPU-stream validation bypass.
 * Success proves fence retirement only. A future combined setup/raster path
 * must also inspect vertex/setup/compaction/raster semantic status before
 * claiming successful rendering. */
static bool nv_surface_shader_raster_dispatch_cpu(u64 owner,
    nv_surface_t *dst, nv_surface_t *z, nv_surface_t *t,
    nv_surface_t *scratch, nv_surface_t *status, const kshr_submission_t *submission) {
    nv_channel_t *ch = &channels[CH_GFX];
    const kshr_job_t *job = &submission->job;
    u32 lanes = kshr_job_lanes(job);
    u64 po = KSHR_JOB_PACKET_OFFSET(job);
    u64 sb = (u64)lanes*sizeof(ksh_status_t);
    if (scratch->bytes < KSHR_JOB_ALLOCATION_BYTES(job) || status->bytes < sb) return false;
    nv_dispatch_layout_t layout;
    /* Job/VM scratch live in a separate surface. QMD/bank2/bank0 use 2 KiB
     * below the method ring; native code occupies its private resident slot. */
    if (!nv_compute_resident_layout(0x400u,24u,&layout) ||
        !ensure_compute_vram(ch)) return false;
    /* The kernel snapshot is copied beyond VM scratch, never into live code or
     * shader-writable registers. The render transaction serializes ALL access
     * to these owner-scoped allocations until retirement. The shader receives
     * only rb scratch bytes, not the packet tail; aliases were rejected above. */
    /* commands is the final member. The shader's two primitive loops are both
     * bounded by the validated command_count, so inactive records need not be
     * retransferred. Retain the FULL allocation/packet capacity and complete
     * header, seed and bytecode; only the inaccessible command tail is stale.
     * One coherent write still completes before any grid may be submitted. */
    _Static_assert(__builtin_offsetof(kshr_job_t,commands)+sizeof(job->commands)==sizeof(*job),
                   "raster commands must remain the final packet member");
    u64 packet_upload = __builtin_offsetof(kshr_job_t,commands) +
                        (u64)job->command_count*sizeof(job->commands[0]);
    if (nv_surface_transfer_ce_ready()) {
        /* The same ordered prepare packet used by GPU geometry setup: clear
         * status on the CE, then upload the active command prefix, with one
         * final completion. Do not upload a CPU zero array or wait twice.
         * Failed preparation must not dispatch or replay through MemUtils. */
        nv_surface_prepare_t prepare[2] = {
            {.dst=status,.bytes=(u32)sb,
             .record_bytes=sizeof(ksh_status_t),.records=lanes},
            {.dst=scratch,.offset=po,.upload=job,.bytes=(u32)packet_upload}};
        if (!nv_surface_prepare_batch(prepare,2u)) return false;
    } else {
        static const ksh_status_t pending[KSHR_FAST_MAX_LANES] = {{0,0}};
        if (!nv_surface_transfer(owner,submission->status,0,(void *)pending,(u32)sb,false) ||
            !nv_surface_transfer(owner,submission->scratch,po,(void *)job,(u32)packet_upload,false)) return false;
    }
    return nv_surface_shader_raster_dispatch_prepared(dst,z,t,scratch,status,job);
}

bool nv_surface_shader_raster(u64 owner, u64 destination, u64 depth, u64 texture,
                              const kshr_submission_t *submission) {
    if (!submission) return false;
    nv_channel_t *ch = &channels[CH_GFX];
    nv_surface_t *dst = nv_surface_lookup(owner,destination);
    nv_surface_t *z = depth ? nv_surface_lookup(owner,depth) : NULL;
    nv_surface_t *t = texture ? nv_surface_lookup(owner,texture) : NULL;
    nv_surface_t *scratch = nv_surface_lookup(owner,submission->scratch);
    nv_surface_t *status = nv_surface_lookup(owner,submission->status);
    nv_surface_t *all[5] = {dst,z,t,scratch,status};
    if (!dst || !scratch || !status || (depth && !z) || (texture && !t) ||
        !ch->open || ch->submit_failed) return false;
    for (u32 a = 0; a < 5u; a++) for (u32 b = a+1u; b < 5u; b++)
        if (all[a] && all[a] == all[b]) return false;
    const kshr_job_t *job = &submission->job;
    if (job->width != dst->width || job->height != dst->height ||
        job->pitch != dst->pitch/4u || job->depth_pitch != (z ? z->pitch/4u : 0u) ||
        (z && (z->width != dst->width || z->height != dst->height)) ||
        job->clip_x >= dst->width || job->clip_y >= dst->height ||
        job->clip_w > dst->width-job->clip_x || job->clip_h > dst->height-job->clip_y ||
        !nv_surface_shader_raster_valid(job,z != NULL,t ? t->bytes : 0u)) return false;
    return nv_surface_shader_raster_dispatch_cpu(owner,dst,z,t,scratch,status,submission);
}

/* Only resolved owner surfaces, never caller-supplied GPU addresses. Both
 * allocations have independent committed mappings in COPY and GR. Retirement
 * is mandatory before a consumer may use the destination command packet. */
static bool nv_surface_copy_bytes(nv_surface_t *src, u64 so,
                                  nv_surface_t *dst, u64 to, u32 bytes) {
    nv_channel_t *copy = &channels[CH_COPY];
    if (!src || !dst || src == dst || src->state != NS_READY || dst->state != NS_READY ||
        src->owner != dst->owner || !src->copy_mapping_attempted || !dst->copy_mapping_attempted ||
        !copy->open || copy->submit_failed || channels[CH_GFX].submit_failed ||
        !bytes || ((so|to|bytes)&3u) || so > src->bytes || bytes > src->bytes-so ||
        to > dst->bytes || bytes > dst->bytes-to) return false;
    return ce_copy(copy,src->va+so,dst->va+to,bytes,bytes,bytes,1u,0x53430000u);
}

/* Clear only a resolved status span. Typed rows avoid the CE's 32768-element
 * line limit without touching padding or neighbouring workspace regions.
 * No CPU zero array, source DMA mapping or status upload is needed. */
static bool nv_surface_clear_records(nv_surface_t *surface, u64 offset,
                                      u32 record_bytes, u32 records) {
    nv_channel_t *copy=&channels[CH_COPY];
    if(!g_render_transaction || !surface || surface->state!=NS_READY ||
        !surface->copy_mapping_attempted || !copy->open || copy->submit_failed ||
        channels[CH_GFX].submit_failed || !record_bytes || !records ||
        ((offset|record_bytes)&3u) || record_bytes/4u>32768u || records>32768u ||
        offset>surface->bytes || (u64)record_bytes*records>surface->bytes-offset)return false;
    u64 bytes=(u64)record_bytes*records;
    /* All raster status arrays fit one line. Avoid a narrow multi-line fill
     * when the exact same span fits the CE element limit. */
    if(bytes<=32768u*4u)
        return ce_fill32(copy,surface->va+offset,(u32)bytes,(u32)bytes/4u,1u,0u,0x535a0000u);
    return ce_fill32(copy,surface->va+offset,record_bytes,record_bytes/4u,
                     records,0u,0x535a0000u);
}

/* Exactly one immutable CPU upload plus bounded surface-to-surface copies or
 * zero fills. All ownership, spans, COPY mappings and staging requirements
 * are checked before the first DMA. The pinned transaction owns the staging
 * allocation until ce_prepare_batch's final fence retires; a failed batch is
 * terminal and is never replayed through the ordinary transfer path. */
static bool nv_surface_prepare_batch(const nv_surface_prepare_t *ops,u32 count) {
    if(!g_render_transaction || !nv_surface_transfer_ce_ready() ||
       channels[CH_GFX].submit_failed || channels[CH_COPY].submit_failed ||
       !ops || !count || count>3u)return false;
    nv_ce_prepare_t packets[3];const void *upload=NULL;u32 upload_bytes=0;
    u64 owner=0;
    for(u32 i=0;i<count;i++) {
        const nv_surface_prepare_t *o=&ops[i];nv_surface_t *d=o->dst,*s=o->src;
        if(!d || d->state!=NS_READY || !d->copy_mapping_attempted ||
           (i&&d->owner!=owner) || !o->bytes || ((o->offset|o->bytes)&3u) ||
           o->offset>d->bytes || o->bytes>d->bytes-o->offset)return false;
        owner=d->owner;
        nv_ce_prepare_t p={.dst=d->va+o->offset,.lines=1u};
        if(o->upload || s) {
            if(o->record_bytes || o->records)return false;
            p.src_pitch=p.dst_pitch=p.line=o->bytes;
            if(o->upload) {
                if(s || upload || o->source_offset || o->bytes>UPLOAD_STAGE_BYTES)return false;
                upload=o->upload;upload_bytes=o->bytes;p.src=VA_UPLOAD_STAGE;
            } else {
                if(s==d || s->state!=NS_READY || !s->copy_mapping_attempted ||
                   s->owner!=owner || (o->source_offset&3u) ||
                   o->source_offset>s->bytes || o->bytes>s->bytes-o->source_offset)return false;
                p.src=s->va+o->source_offset;
            }
        } else {
            if(o->source_offset || !o->record_bytes || !o->records ||
               (o->record_bytes&3u) || o->record_bytes/4u>32768u || o->records>32768u ||
               (u64)o->record_bytes*o->records!=o->bytes)return false;
            p.fill=true;
            if(o->bytes<=32768u*4u)p.src_pitch=p.dst_pitch=o->bytes,p.line=o->bytes/4u;
            else {
                p.src_pitch=p.dst_pitch=o->record_bytes;
                p.line=o->record_bytes/4u;p.lines=o->records;
            }
        }
        packets[i]=p;
    }
    if(!upload)return false;
    memcpy((void *)g_upload_stage,upload,upload_bytes);cache_flush(g_upload_stage,upload_bytes);
    return ce_prepare_batch(&channels[CH_COPY],packets,count);
}

static bool nv_setup_unit_float(const float *f) {
    u32 bits = nv_float_bits(f);
    return (bits&0x7fffffffu) == 0u || (!(bits&0x80000000u) && bits<=0x3f800000u);
}

static bool nv_setup_config_valid(const kshs_config_t *c, bool depth) {
    if (c->version != KSHS_ABI || !c->lanes || c->lanes>KSH_MAX_LANES ||
        !c->triangle_count || c->triangle_count>KSHS_MAX_TRIANGLES ||
        c->first_vertex>c->lanes || c->triangle_count>(c->lanes-c->first_vertex)/3u ||
        c->varying_count>SR_VARYING_N || (c->flat_varying_mask&~((1u<<c->varying_count)-1u)) ||
        !c->framebuffer_width || !c->framebuffer_height ||
        c->framebuffer_width>32768u || c->framebuffer_height>32768u ||
        c->viewport_w<=0 || c->viewport_h<=0 || c->viewport_w>1048576 || c->viewport_h>1048576 ||
        c->viewport_x < -1048576 || c->viewport_x>1048576 ||
        c->viewport_y < -1048576 || c->viewport_y>1048576 ||
        c->clip_x<0 || c->clip_y<0 || c->clip_w<=0 || c->clip_h<=0 ||
        (u32)c->clip_x>=c->framebuffer_width || (u32)c->clip_y>=c->framebuffer_height ||
        (u32)c->clip_w>c->framebuffer_width-(u32)c->clip_x ||
        (u32)c->clip_h>c->framebuffer_height-(u32)c->clip_y ||
        c->clip_x<c->viewport_x || c->clip_y<c->viewport_y ||
        (s64)c->clip_x+c->clip_w>(s64)c->viewport_x+c->viewport_w ||
        (s64)c->clip_y+c->clip_h>(s64)c->viewport_y+c->viewport_h ||
        c->clip_zero_to_one>1u || c->clip_y_down>1u || c->cull_enable>1u ||
        c->front_ccw>1u || c->cull_face>KSHS_CULL_BOTH ||
        !nv_setup_unit_float(&c->depth_near) || !nv_setup_unit_float(&c->depth_far) ||
        c->reserved0 || c->reserved2 || c->reserved3 ||
        c->reserved4[0] || c->reserved4[1]) return false;
    const kg3d_command_t *s = &c->state;
    return !(s->flags&~(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE|KG3D_ALPHA_TEST|KG3D_BLEND)) &&
        (depth || !(s->flags&(KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE))) &&
        s->depth_func<=KG3D_ALWAYS && s->alpha_func<=KG3D_ALWAYS &&
        s->blend_src<=KG3D_ONE_MINUS_DST_ALPHA && s->blend_dst<=KG3D_ONE_MINUS_DST_ALPHA &&
        s->tex_env<=KG3D_DECAL && s->wrap_s<=KG3D_CLAMP && s->wrap_t<=KG3D_CLAMP &&
        s->filter<=KG3D_LINEAR &&
        (nv_float_bits(&s->depth_bias)&0x7f800000u)!=0x7f800000u &&
        (nv_float_bits(&s->alpha_ref)&0x7f800000u)!=0x7f800000u &&
        !s->reserved[0] && !s->reserved[1] && !s->reserved[2];
}

static bool nv_setup_rect_valid(const kshs_rect_t *r, const kshs_config_t *c) {
    return r->x>=c->clip_x && r->y>=c->clip_y && r->width>0 && r->height>0 &&
        (s64)r->x+r->width<=(s64)c->clip_x+c->clip_w &&
        (s64)r->y+r->height<=(s64)c->clip_y+c->clip_h;
}

typedef struct { u32 first,count; kshs_rect_t rect; u64 work; } nv_setup_window_t;
typedef struct { const char *stage; u32 index,result; u64 detail; } nv_setup_failure_t;

static bool nv_setup_reject(const nv_setup_failure_t *why) {
    /* Malformed clients or a repeatedly failing draw must not flood serial or
     * turn diagnostics into a per-frame performance problem. */
    static u32 reports;
    if(reports<16u) {
        reports++;
        kerr("nv-chan","resident geometry rejected: stage=%s index=%u result=%u detail=%llu%s",
            why->stage,why->index,why->result,(unsigned long long)why->detail,
            reports==16u?" (further geometry errors suppressed)":"");
    }
    return false;
}

/* A successful compaction already attests every setup record. Download the
 * larger producer array only to explain a semantic failure, after both stage
 * fences and the compact-status read have retired. Never inspect unfenced GPU
 * memory or replace a failed submission with a diagnostic replay. */
static void nv_setup_failure_detail(u64 owner, u64 workspace, u64 offset,
                                     const kshs_config_t *config, nv_setup_failure_t *why) {
    if(channels[CH_GFX].submit_failed || channels[CH_COPY].submit_failed)return;
    u32 bytes=(u32)KSHS_RESULT_BYTES(config->triangle_count);
    kshs_primitive_t *results=kmalloc(bytes);
    if(!results)return;
    if(nv_surface_transfer(owner,workspace,offset,results,bytes,true)) {
        for(u32 i=0;i<config->triangle_count;i++) {
            if(results[i].result!=KSH_COMPLETE || results[i].count>KSHS_OUTPUT_TRIANGLES) {
                *why=(nv_setup_failure_t){"setup-semantic",i,results[i].result,results[i].count};break;
            }
            bool bad=false;
            for(u32 j=0;j<results[i].count;j++)
                if(!nv_setup_rect_valid(&results[i].bounds[j],config))bad=true;
            if(bad) {
                *why=(nv_setup_failure_t){"setup-rectangle",i,results[i].result,results[i].count};break;
            }
        }
    }
    kfree(results);
}

/* Equivalent to kshr_region_cost for trusted triangle-only commands. Rects
 * are the only geometry metadata downloaded; no vertex/varying is exposed to
 * this planner. Every dimension was checked against the framebuffer first. */
static u64 nv_setup_region_cost(const kshs_rect_t *rects, u32 count, u32 budget,
                                const kshs_rect_t *tile) {
    u64 covered=0;
    for (u32 i=0;i<count;i++) {
        const kshs_rect_t *r=&rects[i];
        s32 x=r->x>tile->x?r->x:tile->x, y=r->y>tile->y?r->y:tile->y;
        s32 right=r->x+r->width<tile->x+tile->width?r->x+r->width:tile->x+tile->width;
        s32 bottom=r->y+r->height<tile->y+tile->height?r->y+r->height:tile->y+tile->height;
        if(right>x && bottom>y)covered+=(u64)(right-x)*(bottom-y);
    }
    u64 tests=(u64)tile->width*tile->height*count, vm=covered*budget;
    return tests>vm?tests:vm;
}

/* Admission runs to completion before the first colour/depth write. A window
 * contains <=64 ordered commands. Disjoint integer tiles retain ordering for
 * blending/depth while bounding both individual and aggregate work. */
static bool nv_setup_plan(const kshs_rect_t *rects, u32 total, kshr_job_t *job,
    nv_surface_t *scratch, nv_surface_t *status, nv_setup_window_t **plan, u32 *plans,
    u32 *maximum_lanes, nv_setup_failure_t *why) {
    u64 work=0;u32 used=0,capacity=0,peak_lanes=0;
    for(u32 first=0;first<total;first+=KG3D_MAX_COMMANDS) {
        u32 count=total-first;if(count>KG3D_MAX_COMMANDS)count=KG3D_MAX_COMMANDS;
        kshs_rect_t bounds=rects[first];
        s32 right=bounds.x+bounds.width,bottom=bounds.y+bounds.height;
        for(u32 i=1;i<count;i++) {
            const kshs_rect_t *r=&rects[first+i];
            if(r->x<bounds.x)bounds.x=r->x;if(r->y<bounds.y)bounds.y=r->y;
            if(r->x+r->width>right)right=r->x+r->width;
            if(r->y+r->height>bottom)bottom=r->y+r->height;
        }
        bounds.width=right-bounds.x;bounds.height=bottom-bounds.y;
        /* At most 30 binary splits along two <=32768 dimensions. */
        kshs_rect_t stack[32];u32 top=0;stack[top++]=bounds;
        while(top) {
            kshs_rect_t tile=stack[--top];
            u64 cost=nv_setup_region_cost(rects+first,count,job->budget,&tile);
            job->clip_x=tile.x;job->clip_y=tile.y;job->clip_w=tile.width;job->clip_h=tile.height;
            job->command_count=count;
            u32 lanes=kshr_job_lanes(job);
            if(cost<=KSH_MAX_WORK && scratch->bytes>=KSHR_JOB_ALLOCATION_BYTES(job) &&
                status->bytes>=(u64)lanes*sizeof(ksh_status_t)) {
                if(used>=KSHS_MAX_RASTER_DISPATCHES || cost>KSHS_MAX_RASTER_WORK-work) {
                    *why=(nv_setup_failure_t){used>=KSHS_MAX_RASTER_DISPATCHES?
                        "admission-dispatch-limit":"admission-whole-work",first,used,work+cost};
                    return false;
                }
                /* All growth precedes framebuffer writes. Keep the old pointer
                 * on allocation failure so the caller can release it. Small
                 * draws no longer allocate a 4096-window worst-case array. */
                if(used==capacity) {
                    u32 next=capacity?capacity*2u:8u;
                    if(next>KSHS_MAX_RASTER_DISPATCHES)next=KSHS_MAX_RASTER_DISPATCHES;
                    nv_setup_window_t *grown=krealloc(*plan,(size_t)next*sizeof(**plan));
                    if(!grown) {
                        *why=(nv_setup_failure_t){"admission-plan-allocation",first,used,next};return false;
                    }
                    *plan=grown;capacity=next;
                }
                (*plan)[used++]=(nv_setup_window_t){first,count,tile,cost};work+=cost;
                if(lanes>peak_lanes)peak_lanes=lanes;
                continue;
            }
            if((tile.width==1 && tile.height==1) || top+2u>32u) {
                *why=(nv_setup_failure_t){"admission-minimum-tile-capacity",first,used,cost};return false;
            }
            kshs_rect_t other=tile;
            if(tile.width>=tile.height && tile.width>1) {
                tile.width/=2;other.x+=tile.width;other.width-=tile.width;
            } else {tile.height/=2;other.y+=tile.height;other.height-=tile.height;}
            stack[top++]=other;stack[top++]=tile;
        }
    }
    *plans=used;*maximum_lanes=peak_lanes;return true;
}

bool nv_surface_shader_geometry(u64 owner,u64 destination,u64 depth,u64 texture,
                                 const kshs_submission_t *submission,u32 *emitted,u64 *executed) {
    if(emitted)*emitted=0;if(executed)*executed=0;
    if(!submission || !emitted || !executed || !g_render_transaction)return false;
    nv_setup_failure_t why={"allocation",0,0,0};
    nv_surface_t *dst=nv_surface_lookup(owner,destination);
    nv_surface_t *z=depth?nv_surface_lookup(owner,depth):NULL;
    nv_surface_t *t=texture?nv_surface_lookup(owner,texture):NULL;
    nv_surface_t *registers=nv_surface_lookup(owner,submission->registers);
    nv_surface_t *vertex_status=nv_surface_lookup(owner,submission->vertex_status);
    nv_surface_t *workspace=nv_surface_lookup(owner,submission->workspace);
    nv_surface_t *scratch=nv_surface_lookup(owner,submission->raster_scratch);
    nv_surface_t *status=nv_surface_lookup(owner,submission->raster_status);
    nv_surface_t *all[8]={dst,z,t,registers,vertex_status,workspace,scratch,status};
    nv_channel_t *ch=&channels[CH_GFX],*copy=&channels[CH_COPY];
    if(!dst || !registers || !vertex_status || !workspace || !scratch || !status ||
        (depth&&!z) || (texture&&!t) || !ch->open || ch->submit_failed ||
        !copy->open || copy->submit_failed) {
        why.stage="surface-or-channel-unavailable";return nv_setup_reject(&why);
    }
    for(u32 a=0;a<8u;a++)for(u32 b=a+1u;b<8u;b++)if(all[a]&&all[a]==all[b]) {
        why=(nv_setup_failure_t){"aliased-surface-roles",a,b,0};return nv_setup_reject(&why);
    }
    const kshs_config_t *c=&submission->setup;
    const kshr_job_t *f=&submission->fragment;
    kshs_storage_layout_t storage;
    if(!nv_setup_config_valid(c,z!=NULL) || !kshs_storage_layout(c->triangle_count,&storage) ||
        workspace->bytes<storage.bytes || registers->bytes<KSHS_REGISTER_BYTES(c->lanes) ||
        vertex_status->bytes<KSHS_VERTEX_STATUS_BYTES(c->lanes) ||
        c->framebuffer_width!=dst->width || c->framebuffer_height!=dst->height ||
        f->width!=dst->width || f->height!=dst->height || f->pitch!=dst->pitch/4u ||
        f->depth_pitch!=(z?z->pitch/4u:0u) || (z&&(z->width!=dst->width || z->height!=dst->height)) ||
        f->clip_x!=(u32)c->clip_x || f->clip_y!=(u32)c->clip_y ||
        f->clip_w!=(u32)c->clip_w || f->clip_h!=(u32)c->clip_h ||
        f->varying_count!=c->varying_count) {
        why.stage="config-dimensions-or-capacity";return nv_setup_reject(&why);
    }

    bool ok=false;u32 triangles=c->triangle_count,total=0,plans=0,maximum_lanes=0;u64 instruction_total=0;
    u32 result_bytes=(u32)KSHS_RESULT_BYTES(triangles);
    u32 compact_bytes=(u32)KSHS_COMPACT_STATUS_BYTES(triangles);
    u32 rect_bytes=(u32)KSHS_COMPACT_RECT_BYTES(triangles*KSHS_OUTPUT_TRIANGLES);
    /* Adjacent bounds/status regions share one fenced snapshot. Never read
     * inactive bounds or use any bounds until every status/prefix validates.
     * The checked internal layout bounds this to < 193 KiB at maximum draw
     * size (one COPY staging chunk); no pixels or vertices return to CPU. */
    u32 metadata_bytes=(u32)(storage.compact-storage.rects)+compact_bytes;
    void *metadata=kmalloc(metadata_bytes);
    kshs_compact_status_t *compact=metadata?
        (kshs_compact_status_t *)((u8 *)metadata+storage.compact-storage.rects):NULL;
    kshs_rect_t *rects=metadata;
    nv_setup_window_t *plan=NULL;
    /* Only the immutable header is read on CPU or uploaded. The entire header
     * is copied below; commands[] remain GPU-produced, so zeroing the unused
     * CPU command tail would be pure per-draw memory traffic. */
    kshr_job_t *job=kmalloc(sizeof(*job));
    ksh_status_t *raster_status=NULL;
    if(!compact || !job)goto done;
    /* The caller's commands/count never become trusted. Probe immutable state
     * with one pixel; actual tile dimensions and work are admitted below. */
    const u32 header_bytes=__builtin_offsetof(kshr_job_t,commands);
    memcpy(job,f,header_bytes);job->command_count=1;job->clip_w=job->clip_h=1;
    why.stage="fragment-state";
    if(!nv_surface_shader_raster_state_valid(job,t?t->bytes:0u))goto done;
    /* All internal workspace spans come from the checked ABI layout. Padding
     * is never exposed as writable capacity to either shader. */
    why.stage="setup-config-upload-status-clear";
    if(nv_surface_transfer_ce_ready()) {
        nv_surface_prepare_t prepare[3]={
            {.dst=workspace,.offset=storage.config,.upload=c,.bytes=sizeof(*c)},
            {.dst=workspace,.offset=storage.primitives,.bytes=result_bytes,
             .record_bytes=sizeof(kshs_primitive_t),.records=triangles},
            {.dst=workspace,.offset=storage.compact,.bytes=compact_bytes,
             .record_bytes=sizeof(kshs_compact_status_t),.records=triangles}};
        if(!nv_surface_prepare_batch(prepare,3u))goto done;
    } else if(!nv_surface_transfer(owner,submission->workspace,storage.config,(void *)c,sizeof(*c),false) ||
              !nv_surface_clear_records(workspace,storage.primitives,sizeof(kshs_primitive_t),triangles) ||
              !nv_surface_clear_records(workspace,storage.compact,sizeof(kshs_compact_status_t),triangles))goto done;
    u64 setup_values[12]={registers->va,vertex_status->va,workspace->va+storage.commands,
        workspace->va+storage.primitives,workspace->va+storage.clip,workspace->va+storage.config,
        KSHS_REGISTER_BYTES(c->lanes),KSHS_VERTEX_STATUS_BYTES(c->lanes),KSHS_COMMAND_BYTES(triangles),
        result_bytes,KSHS_SCRATCH_BYTES(triangles),sizeof(*c)};
    u32 setup_args[24];memcpy(setup_args,setup_values,sizeof(setup_args));
    /* Compaction validates ALL setup statuses/counts (cooperatively per warp
     * for larger batches, independently per lane for tiny batches) and
     * validates each active source rectangle against its generated command.
     * Only after every compact lane completes may its packed stream be used.
     * Inactive setup bounds/commands are never consumed. */
    u64 compact_values[10]={workspace->va+storage.commands,workspace->va+storage.primitives,
        workspace->va+storage.packed,workspace->va+storage.rects,workspace->va+storage.compact,
        KSHS_COMMAND_BYTES(triangles),result_bytes,KSHS_COMMAND_BYTES(triangles),rect_bytes,compact_bytes};
    u32 compact_args[21];memcpy(compact_args,compact_values,sizeof(compact_values));compact_args[20]=triangles;
    why=(nv_setup_failure_t){"setup-compact-dispatch-or-readback",0,0,total};
    /* Preserve the measured tiny-draw shader; larger batches share count-table
     * reads across each warp. Both use the same ABI and mandatory retirement. */
    if(!nv_compute_geometry_pair(ch,setup_args,compact_args,triangles) ||
        !nv_surface_transfer(owner,submission->workspace,storage.rects,metadata,metadata_bytes,true))goto done;
    total=compact[0].total;
    u32 prefix=0;
    for(u32 i=0;i<triangles;i++) {
        why=(nv_setup_failure_t){"compact-semantic-prefix",i,compact[i].result,
            ((u64)compact[i].first<<32)|compact[i].total};
        if(compact[i].result!=KSH_COMPLETE || compact[i].count>KSHS_OUTPUT_TRIANGLES ||
            total>triangles*KSHS_OUTPUT_TRIANGLES || compact[i].first!=prefix || compact[i].total!=total ||
            prefix>total || compact[i].count>total-prefix) {
            nv_setup_failure_detail(owner,submission->workspace,storage.primitives,c,&why);goto done;
        }
        prefix+=compact[i].count;
    }
    if(prefix!=total)goto done;
    if(!total){ok=true;goto done;}
    /* The snapshot includes capacity padding/unused bounds, which remain
     * untrusted and unconsumed. Only this validated active prefix is planned.
     * Fully culled batches still allocate no plan or raster-status snapshot. */
    for(u32 i=0;i<total;i++) {
        why=(nv_setup_failure_t){"compact-rectangle-bounds",i,0,total};
        if(!nv_setup_rect_valid(&rects[i],c))goto done;
    }
    if(!nv_setup_plan(rects,total,job,scratch,status,&plan,&plans,&maximum_lanes,&why))goto done;
    why=(nv_setup_failure_t){"raster-status-allocation",0,maximum_lanes,plans};
    if(!plans || !maximum_lanes || maximum_lanes>KSHR_FAST_MAX_LANES)goto done;
    raster_status=kmalloc((size_t)maximum_lanes*sizeof(*raster_status));
    if(!raster_status)goto done;
    /* No colour/depth write occurred before this point. Setup and compaction
     * have retired; every command has trusted provenance under the same guard.
     * Only the header is CPU-uploaded. Commands move directly VRAM-to-VRAM. */
    for(u32 i=0;i<plans;i++) {
        const nv_setup_window_t *w=&plan[i];
        job->command_count=w->count;job->clip_x=w->rect.x;job->clip_y=w->rect.y;
        job->clip_w=w->rect.width;job->clip_h=w->rect.height;
        u32 lanes=kshr_job_lanes(job),sb=lanes*sizeof(*raster_status);
        u64 po=KSHR_JOB_PACKET_OFFSET(job);
        why=(nv_setup_failure_t){"raster-window-transfer-or-fence",i,w->count,w->work};
        if(lanes>maximum_lanes)goto done;
        if(nv_surface_transfer_ce_ready()) {
            nv_surface_prepare_t prepare[3]={
                {.dst=status,.bytes=sb,.record_bytes=sizeof(ksh_status_t),.records=lanes},
                {.dst=scratch,.offset=po,.upload=job,.bytes=header_bytes},
                {.dst=scratch,.offset=po+header_bytes,.src=workspace,
                 .source_offset=storage.packed+(u64)w->first*sizeof(kshr_command_t),
                 .bytes=w->count*sizeof(kshr_command_t)}};
            if(!nv_surface_prepare_batch(prepare,3u))goto done;
        } else if(!nv_surface_clear_records(status,0,sizeof(ksh_status_t),lanes) ||
                  !nv_surface_transfer(owner,submission->raster_scratch,po,job,header_bytes,false) ||
                  !nv_surface_copy_bytes(workspace,storage.packed+(u64)w->first*sizeof(kshr_command_t),
                      scratch,po+header_bytes,w->count*sizeof(kshr_command_t)))goto done;
        if(!nv_surface_shader_raster_dispatch_prepared(dst,z,t,scratch,status,job) ||
            !nv_surface_transfer(owner,submission->raster_status,0,raster_status,sb,true))goto done;
        u64 pixels=(u64)job->clip_w*job->clip_h,window_executed=0;
        for(u32 lane=0;lane<lanes;lane++) {
            u64 lane_pixels=(pixels-1u-lane)/lanes+1u;
            why=(nv_setup_failure_t){"raster-semantic",lane,raster_status[lane].result,
                ((u64)i<<32)|raster_status[lane].executed};
            if(raster_status[lane].result!=KSH_COMPLETE ||
                raster_status[lane].executed>lane_pixels*w->count*job->budget)goto done;
            window_executed+=raster_status[lane].executed;
        }
        why=(nv_setup_failure_t){"raster-executed-bound",i,0,window_executed};
        if(window_executed>w->work || instruction_total>KSHS_MAX_RASTER_WORK-window_executed)goto done;
        instruction_total+=window_executed;
    }
    *emitted=total;*executed=instruction_total;ok=true;
done:
    /* These are CPU-only snapshots/plans, never DMA buffers. GPU allocations
     * remain owned by their handles; existing failed-channel teardown retains
     * and quarantines them. No alternate renderer/replay follows failure. A
     * retired semantic error can leave prior tiles written, never claims rollback. */
    kfree(raster_status);kfree(job);kfree(plan);kfree(metadata);
    return ok?true:nv_setup_reject(&why);
}

bool nv_surface_present(u64 owner, u64 source,
                        u32 view_x, u32 view_y, u32 view_w, u32 view_h,
                        u32 x, u32 y, u32 width, u32 height, u32 rotation) {
    nv_surface_t *src = nv_surface_lookup(owner, source);
    nv_channel_t *ch = &channels[CH_GFX];
    u32 dw = g_runtime_scanout_width, dh = g_runtime_scanout_height;
    if (!src || (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) ||
        !ch->open || ch->submit_failed || !g_scanout_bound ||
        !g_compute_scanout_bound || !dw || !dh || dw > 32768u || dh > 32768u ||
        (g_runtime_scanout_pitch & 3u) || g_runtime_scanout_pitch / 4u < dw ||
        (u64)g_runtime_scanout_pitch * dh > g_runtime_scanout_bytes ||
        !view_w || !view_h || view_x >= src->width || view_y >= src->height ||
        view_w > src->width - view_x || view_h > src->height - view_y ||
        !width || !height || x >= dw || y >= dh || width > dw-x || height > dh-y)
        return false;
    /* gui_present is an exact word copy when there is no scale or rotation:
     * source=(view_x+x, view_y+y). Both surfaces already have committed COPY
     * mappings, and the preceding GR draw retired before this transaction.
     * Avoid the QMD/constant upload and compute launch for ordinary desktop
     * damage. Retain one flushed CE release before scanout reuse/next drawing.
     * A failed copy is terminal here, never retried through the shader. */
    if (g_render_transaction && !rotation && view_w == dw && view_h == dh &&
        src->copy_mapping_attempted) {
        nv_channel_t *copy = &channels[CH_COPY];
        if (!copy->open || copy->submit_failed || copy->card != ch->card)
            return false;
        u64 source_va = src->va + (u64)(view_y + y) * src->pitch +
                     (u64)(view_x + x) * 4u;
        u64 target = g_runtime_scanout_va + (u64)y * g_runtime_scanout_pitch +
                     (u64)x * 4u;
        return ce_copy(copy, source_va, target, src->pitch, g_runtime_scanout_pitch,
                        width * 4u, height, 0x47500000u);
    }
    u32 args[19] = {
        (u32)g_runtime_scanout_va, (u32)(g_runtime_scanout_va >> 32),
        (u32)src->va, (u32)(src->va >> 32),
        src->width, src->height, src->pitch / 4u,
        dw, dh, g_runtime_scanout_pitch / 4u,
        view_x, view_y, view_w, view_h, x, y, width, height, rotation
    };
    u32 grid[3] = {(width + 15u) / 16u, (height + 15u) / 16u, 1u};
    u32 block[3] = {16u, 16u, 1u};
    return nv_compute_dispatch_one(ch, gui_present_sass, sizeof gui_present_sass,
                                   gui_present_sass_reg_count, 0xc000u,
                                   args, 19u, grid, block, 0x47500000u, "surface/present");
}

/* Prove the compute engine executes a real sm_120 shader on the card.  Two
 * dispatches: (1) a NOP kernel - CUDA ABI prologue plus EXIT, no global-memory
 * access, so it cannot
 * fault on the undetermined global-store descriptor; its completion semaphore
 * firing is the honest proof that GR/compute dispatched and RAN on silicon.
 * (2) best-effort: writeval stores 0xCAFEBABE via that descriptor (left 0) -
 * if the buffer reads back the value the data path works too, but execution is
 * settled by (1) regardless.  Returns 0 iff the execution proof (1) passed. */
int nv_compute_selftest_hw(void) {
    nv_channel_t *ch = &channels[CH_GFX];
    if (!ch->open) { kwarn("nv-chan", "no compute channel to run a shader on"); return -1; }

    bool exec = nv_compute_dispatch_one(ch, gpunop_sass, (u32)sizeof gpunop_sass,
                                        gpunop_sass_reg_count, 0x8000, NULL, 0,
                                        NULL, NULL, 0xC0DE0001u, "nop/exec");
    if (exec)
        kinfo("nv-chan", "COMPUTE runs on the real card: a real sm_120 shader "
                         "dispatched and ran to completion (GR execution proven)");
    else
        kerr("nv-chan", "compute dispatch did not complete - see the RAMFC/doorbell "
                        "diagnostics above for whether it was fetch, schedule or context");

    /* Data path (best-effort; store descriptor unknown). */
    volatile u32 *out = (volatile u32 *)(ch->pushbuf + 0xC000);
    *out = 0;
    __asm__ volatile("mfence" ::: "memory");
    u32 wv_args[2] = { (u32)(VA_PUSHBUF + 0xC000), (u32)((VA_PUSHBUF + 0xC000) >> 32) };
    bool ran = nv_compute_dispatch_one(ch, writeval_sass, (u32)sizeof writeval_sass,
                                       writeval_sass_reg_count, 0xA000,
                                       wv_args, 2, NULL, NULL, 0xC0DE0002u, "writeval/data");
    cache_flush((const void *)out, 4);
    u32 got = *out;
    if (ran && got == 0xCAFEBABEu)
        kinfo("nv-chan", "COMPUTE data path works: the kernel wrote %#x to memory", got);
    else
        kwarn("nv-chan", "compute data path unproven: buffer=%#x (want CAFEBABE); the "
                         "global-store descriptor c[0x0][0x358] is undetermined and left "
                         "0 - execution is proven by the nop, the store is not", got);
    return exec ? 0 : 1;   /* 0 = pass, 1 = ran-but-failed (channel-not-open returns -1 above) */
}

/* A displayed frame can legitimately remain unchanged for seconds.  Keep the
 * host-RM CE and GR channels resident during that interval without touching
 * the live scanout: CE copies one cache line between the private staging and
 * scratch mappings, while GR executes the already-proven one-thread NOP grid.
 * Both operations carry completion fences, so returning true also proves no
 * transient allocation is still in flight. */
bool nv_chan_render_keepalive(void) {
    nv_channel_t *copy = &channels[CH_COPY];
    nv_channel_t *gfx = &channels[CH_GFX];
    if (!copy->open || !gfx->open || copy->submit_failed || gfx->submit_failed ||
        !g_upload_stage_ready ||
        !g_present_scratch_ready || !ensure_compute_vram(gfx))
        return false;

    volatile u32 *stage = (volatile u32 *)g_upload_stage;
    for (u32 i = 0; i < 64u; i++) stage[i] = 0x4b455354u ^ i;
    cache_flush(stage, 256u);
    if (!ce_copy(copy, VA_UPLOAD_STAGE, VA_PRESENT_SCRATCH,
                 256u, 256u, 256u, 1u, 0x4b410000u))
        return false;

    if (!pb_reserve(gfx, 128u)) return false;
    u32 one[3] = { 1u, 1u, 1u };
    return nv_compute_dispatch_one(gfx, gpunop_sass, (u32)sizeof gpunop_sass,
                                   gpunop_sass_reg_count, 0x8000, NULL, 0,
                                   one, one, 0x4b420000u, "dwell/keepalive");
}

/* ---------------------------------------------- 3D via the compute rasterizer
 * Draw a filled triangle into a VRAM surface with a real sm_120 compute shader
 * (tri_raster), then read the surface back via PRAMIN and byte-verify it.  This
 * is 3D rendering that rides entirely on the proven compute-dispatch path - it
 * needs NONE of the 0xce97 graphics pipeline (whose VS/FS microcode ptxas cannot
 * emit).  One thread per pixel; a pixel is filled iff it is inside the triangle. */
int nv_3d_raster_selftest_hw(void) {
    nv_channel_t *ch = &channels[CH_GFX];
    if (!ch->open) { kwarn("nv-chan", "3D-raster: no compute channel"); return -1; }

    const u32 W = 64, H = 64, pitch = 64, bytes = 64 * 64 * 4;   /* 0x4000 = 16 KiB */
    const u32 color = 0xFFFF0000u;                 /* opaque red */
    /* Reuse the tail of the already-mapped compute VRAM buffer (VA_COMPUTE, 64 KiB):
     * the dispatch only uses [0x9000,0x9C00), so [0xC000,0x10000) = 16 KiB is free
     * and exactly fits a 64x64x4 surface.  Avoids a fragile second VRAM alloc. */
    if (!ensure_compute_vram(ch)) { kwarn("nv-chan", "3D-raster: compute VRAM not ready"); return -1; }
    const u64 VA_RASTER = VA_COMPUTE + 0xC000;
    static u32 surf[64 * 64];
    memset(surf, 0, sizeof surf);
    if (!nv_vram_object_write(ch, H_COMPUTE_VRAM, g_compute_vram_fb,
                              0xC000, surf, bytes)) {
        kerr("nv-chan", "3D-raster: coherent surface clear failed");
        return 1;
    }

    /* Args in declaration order at c[0x0][0x380]: fb(8B), W,H,pitch, a/b/c, color. */
    u32 args[12] = { (u32)VA_RASTER, (u32)(VA_RASTER >> 32),
                     W, H, pitch, 32, 8, 8, 56, 56, 56, color };
    u32 grid[3]  = { (W + 15) / 16, (H + 15) / 16, 1 };
    u32 block[3] = { 16, 16, 1 };
    bool ran = nv_compute_dispatch_one(ch, tri_raster_sass, (u32)sizeof tri_raster_sass,
                                       tri_raster_sass_reg_count, 0x9000,
                                       args, 12, grid, block, 0xC0DE0003u, "3d/raster");
    if (!ran) { kerr("nv-chan", "3D-raster: the rasterizer grid did not complete"); return 1; }

    /* Read the whole surface back into a CPU buffer: (a) write it to the USB as a
     * raw 64x64 BGRA image so the pixels the GPU actually drew can be turned into
     * a picture, and (b) log it as ASCII art so the triangle is visible right in
     * the boot log. */
    if (!nv_vram_object_read(ch, H_COMPUTE_VRAM, g_compute_vram_fb,
                             0xC000, surf, bytes)) {
        kerr("nv-chan", "3D-raster: coherent surface readback failed");
        return 1;
    }
    u32 inside = surf[40u * pitch + 32u];
    u32 corner = surf[0];
    u32 filled = 0;
    for (u32 y = 0; y < H; y++) for (u32 x = 0; x < W; x++) {
        u32 px = surf[y * W + x];
        if (px == color) filled++;
    }
    int vfs_write_file(const char *path, const void *data, unsigned long len);
    (void)vfs_write_file("/boot/KESTREL/tri.raw", surf, (unsigned long)(W * H * 4));
    kinfo("nv-chan", "3D-raster: wrote the %ux%u rendered surface to /boot/KESTREL/tri.raw", W, H);
    /* ASCII art: sample the 64x64 surface down to 32 wide for a legible shape. */
    for (u32 y = 0; y < H; y += 2) {
        char row[35]; u32 n = 0;
        for (u32 x = 0; x < W; x += 2) row[n++] = surf[y * W + x] == color ? '#' : '.';
        row[n] = 0;
        kinfo("nv-chan", "3D| %s", row);
    }
    kinfo("nv-chan", "3D-raster: inside=%#x corner=%#x filled=%u/%u px",
          inside, corner, filled, W * H);
    if (inside == color && corner == 0 && filled > 100 && filled < W * H) {
        kinfo("nv-chan", "3D RENDERS on the real card: the compute rasterizer drew and "
                         "byte-verified a filled triangle (%u px) - hardware 3D output "
                         "works WITHOUT the graphics pipeline", filled);
        return 0;
    }
    kwarn("nv-chan", "3D-raster: triangle did not verify (inside %#x corner %#x filled %u)",
          inside, corner, filled);
    return 1;
}

/* Production triangle submission used by SYS_GPU.  Kestrel's public vertex
 * ABI supplies clip-space x/y plus RGBA.  Convert each triangle to the integer
 * arguments consumed by the already hardware-verified sm_120 compute
 * rasterizer and point that shader at the live scanout mapping.  This is a
 * genuine GPU raster path (SM work writes display VRAM), even though it avoids
 * the still-undocumented CE97 vertex/fragment program ABI. */
/* Decode a finite IEEE-754 value to signed Q16 without using floating-point in
 * the kernel (it is built -mgeneral-regs-only and deliberately has no soft-fp
 * runtime).  Values outside [-1,1], infinities and NaNs are clamped. */
static s32 fbits_to_q16(u32 b) {
    bool neg = (b >> 31) != 0;
    u32 exp = (b >> 23) & 0xffu, frac = b & 0x7fffffu;
    if (exp >= 127u) return neg ? -65536 : 65536;
    if (exp < 111u) return 0;                    /* magnitude < 2^-16 */
    u32 mant = frac | 0x800000u;
    u32 shift = 134u - exp;                     /* IEEE mantissa -> Q16 */
    s32 q = shift < 32u ? (s32)(mant >> shift) : 0;
    return neg ? -q : q;
}
static u32 fbits(const float *p) { u32 b; memcpy(&b, p, sizeof b); return b; }
static s32 clip_to_pixel_bits(u32 bits, u32 extent, bool invert) {
    s32 q = fbits_to_q16(bits);
    s32 n = invert ? (65536 - q) : (q + 65536); /* Q16 in [0,2] */
    return (s32)(((u64)(u32)n * (u64)(extent ? extent - 1u : 0u)) >> 17);
}
static u32 q16_byte(s32 q) {
    if (q < 0) q = 0; if (q > 65536) q = 65536;
    return (u32)(((u64)(u32)q * 255u + 32768u) >> 16);
}
static u32 chan_colour_bits(const float *a, const float *b, const float *c) {
    s32 r = (fbits_to_q16(fbits(a + 4)) + fbits_to_q16(fbits(b + 4)) +
             fbits_to_q16(fbits(c + 4))) / 3;
    s32 g = (fbits_to_q16(fbits(a + 5)) + fbits_to_q16(fbits(b + 5)) +
             fbits_to_q16(fbits(c + 5))) / 3;
    s32 bl= (fbits_to_q16(fbits(a + 6)) + fbits_to_q16(fbits(b + 6)) +
             fbits_to_q16(fbits(c + 6))) / 3;
    s32 al= (fbits_to_q16(fbits(a + 7)) + fbits_to_q16(fbits(b + 7)) +
             fbits_to_q16(fbits(c + 7))) / 3;
    return (q16_byte(al) << 24) | (q16_byte(r) << 16) |
           (q16_byte(g) << 8) | q16_byte(bl);
}

/* Prepare several independent raster QMDs in one contiguous VRAM upload, then
 * launch them in order through one GPFIFO entry and retire them with one fence.
 * The old path paid an RM transfer, doorbell and CPU wait for every triangle;
 * this keeps the public ABI but amortizes those costs over a small batch. */
#define TRI_BATCH_MAX       8u
#define TRI_BATCH_STRIDE    0xC00u
static bool nv_compute_raster_batch(nv_channel_t *ch, const float *v,
                                    u32 triangles, u32 W, u32 H) {
    if (!triangles || triangles > TRI_BATCH_MAX || ch->submit_failed ||
        !ensure_compute_vram(ch))
        return false;
    const u32 bytes = triangles * TRI_BATCH_STRIDE;
    volatile u8 *base = ch->pushbuf;
    memset((void *)base, 0, bytes);

    for (u32 t = 0; t < triangles; t++) {
        const u32 data_off = t * TRI_BATCH_STRIDE;
        const u32 qmd_off = data_off;
        const u32 sh_off = data_off + 0x400u;
        const u32 cb_off = data_off + 0x800u;
        const float *p = v + (u64)t * 24u;
        const u64 qmd_va = VA_COMPUTE + qmd_off;
        const u64 sh_va = VA_COMPUTE + sh_off;
        const u64 cb_va = VA_COMPUTE + cb_off;

        memcpy((void *)(base + sh_off), tri_raster_sass,
               (u32)sizeof tri_raster_sass);
        *(volatile u32 *)(base + cb_off + 0x2f0) = 0u;
        *(volatile u32 *)(base + cb_off + 0x2f4) = 1u;
        *(volatile u32 *)(base + cb_off + 0x2f8) = 0xff000000u;
        *(volatile u32 *)(base + cb_off + 0x2fc) = 0u;
        *(volatile u32 *)(base + cb_off + 0x360) = 16u;
        *(volatile u32 *)(base + cb_off + 0x364) = 16u;
        *(volatile u32 *)(base + cb_off + 0x368) = 1u;
        *(volatile u32 *)(base + cb_off + 0x37c) = 0x00fffdc0u;

        u32 args[12] = {
            (u32)g_runtime_scanout_va, (u32)(g_runtime_scanout_va >> 32),
            W, H, g_runtime_scanout_pitch / 4u,
            (u32)clip_to_pixel_bits(fbits(p + 0), W, false),
            (u32)clip_to_pixel_bits(fbits(p + 1), H, true),
            (u32)clip_to_pixel_bits(fbits(p + 8), W, false),
            (u32)clip_to_pixel_bits(fbits(p + 9), H, true),
            (u32)clip_to_pixel_bits(fbits(p + 16), W, false),
            (u32)clip_to_pixel_bits(fbits(p + 17), H, true),
            chan_colour_bits(p, p + 8, p + 16),
        };
        for (u32 i = 0; i < 12u; i++)
            *(volatile u32 *)(base + cb_off + 0x380u + i * 4u) = args[i];

        u32 qmd[NV_QMD_WORDS];
        nv_compute_launch_t l = { 0 };
        l.program_addr = sh_va;
        l.grid[0] = (W + 15u) / 16u; l.grid[1] = (H + 15u) / 16u; l.grid[2] = 1u;
        l.block[0] = 16u; l.block[1] = 16u; l.block[2] = 1u;
        l.reg_count = tri_raster_sass_reg_count;
        l.program_size = (u32)sizeof tri_raster_sass;
        l.sass_version = 0xa4; l.sampler_index = 1u;
        l.barrier_count = 1u; l.qmd_group_id = 0x3fu;
        nv_qmd_build_compute(qmd, &l);
        nv_qmd_set_constant_buffer(qmd, 0, cb_va, 0x400u);
        for (u32 i = 0; i < NV_QMD_WORDS; i++)
            *(volatile u32 *)(base + qmd_off + i * 4u) = qmd[i];
        (void)qmd_va;
    }

    if (!nv_vram_object_write(ch, H_COMPUTE_VRAM, g_compute_vram_fb,
                              0, (const void *)base, bytes)) {
        kerr("nv-chan", "runtime/3d batch: coherent QMD/program upload failed");
        return false;
    }
    if (!pb_reserve(ch, 256u)) return false;
    u32 sentinel = next_completion_signal(ch, 0x3344b000u);
    ch->sem[0] = 0;
    __asm__ volatile("mfence" ::: "memory");
    u32 start = ch->pb_at;
    pb_method(ch, SUBCH_COMPUTE, CEC0_SET_OBJECT, 1); pb_data(ch, BLACKWELL_COMPUTE_B);
    pb_method(ch, SUBCH_COMPUTE, CEC0_INVALIDATE_SKED_CACHES, 1); pb_data(ch, 0);
    pb_method(ch, SUBCH_COMPUTE, CEC0_SET_SHADER_SHARED_MEMORY_WINDOW_A, 2);
        pb_data(ch, 1u); pb_data(ch, 0u);
    pb_method(ch, SUBCH_COMPUTE, CEC0_SET_SHADER_LOCAL_MEMORY_WINDOW_A, 2);
        pb_data(ch, 0u); pb_data(ch, 0xff000000u);
    for (u32 t = 0; t < triangles; t++) {
        u64 qmd_va = VA_COMPUTE + (u64)t * TRI_BATCH_STRIDE;
        pb_method(ch, SUBCH_COMPUTE, CEC0_SEND_PCAS_A, 1);
            pb_data(ch, (u32)(qmd_va >> 8));
        pb_method(ch, SUBCH_COMPUTE, CEC0_SEND_PCAS2_B, 1);
            pb_data(ch, CEC0_PCAS2_ACTION_ICS);
    }
    pb_method(ch, SUBCH_COMPUTE, CEC0_WAIT_FOR_IDLE, 1); pb_data(ch, 0);
    pb_method(ch, SUBCH_COMPUTE, CEC0_REPORT_SEM_ADDR_LO, 2);
        pb_data(ch, (u32)VA_SEM); pb_data(ch, (u32)(VA_SEM >> 32));
    pb_method(ch, SUBCH_COMPUTE, CEC0_REPORT_SEM_PAY_LO, 1); pb_data(ch, sentinel);
    pb_method(ch, SUBCH_COMPUTE, CEC0_REPORT_SEM_EXECUTE, 1);
        pb_data(ch, CEC0_REPORT_SEM_RELEASE_1W |
                    nv_completion_awaken(ch, COMPUTE_COMPLETION_AWAKEN));
    return submit_and_wait(ch, start, sentinel);
}

int nv_chan_draw_triangles(const float *v, u32 triangles) {
    nv_channel_t *ch = &channels[CH_GFX];
    if (!v || !triangles || !ch->open || ch->submit_failed || !g_scanout_bound ||
        !g_compute_scanout_bound || !ensure_compute_vram(ch)) return -1;
    u32 W = g_runtime_scanout_width, H = g_runtime_scanout_height;
    u32 done = 0;
    while (done < triangles) {
        u32 batch = triangles - done;
        if (batch > TRI_BATCH_MAX) batch = TRI_BATCH_MAX;
        u64 started = timer_now_us();
        if (!nv_compute_raster_batch(ch, v + (u64)done * 24u, batch, W, H))
            return (int)done;
        u64 elapsed = timer_now_us() - started;
        kinfo("nv-chan", "runtime/3d batch: %u triangle(s), one upload/submission/fence, %llu us",
              batch, (unsigned long long)elapsed);
        done += batch;
    }
    return (int)done;
}

/* Exercise the exact primitives used after the GUI takes ownership, against
 * the live scanout allocation rather than a private laboratory buffer. */
int nv_chan_runtime_selftest_hw(void) {
    if (!nv_chan_display_ready() || g_runtime_scanout_width < 64u ||
        g_runtime_scanout_height < 64u) return -1;
    const u32 a = 0xff13579bu, b = 0xff2468acu;
    static const u32 image4[16] = {
        0xff102030u,0xff112131u,0xff122232u,0xff132333u,
        0xff203040u,0xff213141u,0xff223242u,0xff233343u,
        0xff304050u,0xff314151u,0xff324252u,0xff334353u,
        0xff405060u,0xff415161u,0xff425262u,0xff435363u,
    };
    if (!nv_chan_fill_scanout(8, 8, 8, 8, a) ||
        !nv_chan_fill_scanout(24, 8, 8, 8, b) ||
        !nv_chan_copy_scanout(8, 8, 40, 8, 8, 8) ||
        !nv_chan_present_image(image4, 4, 4, 4, 56, 8)) return 1;
    { u32 fl = 0x3u;
      (void)nv_rm_control(channels[CH_COPY].card, channels[CH_COPY].rm,
                          RM_SUBDEVICE, NV2080_CTRL_CMD_FB_FLUSH_GPU_CACHE_IRQL,
                          &fl, sizeof fl, NULL, 0, NULL); }
    u64 p0 = g_runtime_scanout_fb + 8ull * g_runtime_scanout_pitch + 8u * 4u;
    u64 p1 = g_runtime_scanout_fb + 8ull * g_runtime_scanout_pitch + 40u * 4u;
    u64 p2 = g_runtime_scanout_fb + 11ull * g_runtime_scanout_pitch + 59u * 4u;
    u32 got0 = nv_fb_rd32(channels[CH_COPY].card, p0);
    u32 got1 = nv_fb_rd32(channels[CH_COPY].card, p1);
    u32 got2 = nv_fb_rd32(channels[CH_COPY].card, p2);
    if (got0 != a || got1 != a || got2 != image4[15]) {
        kwarn("nv-chan", "runtime 2D/present readback did not match: fill=%#x/%#x "
                         "move=%#x/%#x image=%#x/%#x",
              got0, a, got1, a, got2, image4[15]);
        return 1;
    }
    /* IEEE-754 bit patterns avoid all floating-point execution in the kernel:
     * (-.5,-.5),(.5,-.5),(0,.5), opaque magenta at every vertex. */
    static const u32 tri[24] = {
        0xbf000000u,0xbf000000u,0,0x3f800000u, 0x3f800000u,0,0x3f800000u,0x3f800000u,
        0x3f000000u,0xbf000000u,0,0x3f800000u, 0x3f800000u,0,0x3f800000u,0x3f800000u,
        0,0x3f000000u,0,0x3f800000u,          0x3f800000u,0,0x3f800000u,0x3f800000u,
    };
    if (nv_chan_draw_triangles((const float *)tri, 1) != 1) return 1;
    { u32 fl = 0x3u;
      (void)nv_rm_control(channels[CH_COPY].card, channels[CH_COPY].rm,
                          RM_SUBDEVICE, NV2080_CTRL_CMD_FB_FLUSH_GPU_CACHE_IRQL,
                          &fl, sizeof fl, NULL, 0, NULL); }
    u64 pc = g_runtime_scanout_fb + (u64)(g_runtime_scanout_height / 2u) *
             g_runtime_scanout_pitch + (u64)(g_runtime_scanout_width / 2u) * 4u;
    if (nv_fb_rd32(channels[CH_COPY].card, pc) != 0xffff00ffu) {
        kwarn("nv-chan", "runtime 3D-to-scanout pixel did not match");
        return 1;
    }
    kinfo("nv-chan", "runtime GUI path PASS: CAB5 fill/copy/present and SM triangle wrote live scanout");
    return 0;
}

/* Deliberately visible full-screen engine proofs.  The NVKMS client binds each
 * head's inactive scanout before calling these routines, then flips all heads
 * together only after every surface is complete.  This keeps partially drawn
 * frames off the wire and makes the physical monitor—not an offscreen buffer—
 * the test output. */
int nv_chan_visible_2d_hw(void) {
    if (!nv_chan_display_ready() || g_runtime_scanout_width < 320u ||
        g_runtime_scanout_height < 240u)
        return -1;

    const u32 w = g_runtime_scanout_width, h = g_runtime_scanout_height;
    const s32 margin = (s32)(w / 16u);
    const s32 banner_x = (s32)(w / 8u);
    const s32 banner_y = (s32)(h / 16u);
    const s32 banner_w = (s32)(w * 3u / 4u);
    const s32 banner_h = (s32)(h / 8u);
    static const u32 cards[6] = {
        0xffff3b30u, 0xffff9500u, 0xffffcc00u,
        0xff34c759u, 0xff007affu, 0xffaf52deu,
    };

    if (!nv_chan_fill_scanout(0, 0, (s32)w, (s32)h, 0xff101827u) ||
        !nv_chan_fill_scanout(banner_x, banner_y, banner_w, banner_h,
                              0xff19d3ffu))
        return 1;

    /* Six independently issued rectangles make the CE work visually obvious.
     * Their dimensions derive from the current monitor, so this is not tied to
     * the three 2560x1440 panels used during bring-up. */
    const s32 gap = (s32)(w / 64u);
    const s32 card_y = (s32)(h * 5u / 16u);
    const s32 card_h = (s32)(h / 4u);
    const s32 usable = (s32)w - 2 * margin - 5 * gap;
    const s32 card_w = usable / 6;
    for (u32 i = 0; i < 6; i++)
        if (!nv_chan_fill_scanout(margin + (s32)i * (card_w + gap), card_y,
                                  card_w, card_h, cards[i]))
            return 1;

    /* A real VRAM->VRAM copy, not a second fill, repeats the cyan banner near
     * the bottom.  The source and destination do not overlap. */
    if (!nv_chan_copy_scanout(banner_x, banner_y, banner_x,
                              (s32)(h * 13u / 16u), banner_w, banner_h))
        return 1;

    /* Exercise the first production desktop transfer exactly: a narrow 32x32
     * damage upload with a 128-byte source pitch.  The old launch word passed
     * every large proof above, then this shape was the first CAB5 command to
     * fault after userland started.  Put a unique colour over the centre of
     * the banner so the existing KAPI MemUtils readback proves this particular
     * small transfer, rather than merely proving the full-width fill beneath
     * it. */
    static u32 small_damage[32u * 32u];
    for (u32 i = 0; i < 32u * 32u; i++) small_damage[i] = 0xff31e6a8u;
    if (!nv_chan_present_image(small_damage, 32u, 32u, 32u,
                               (s32)(w / 2u) - 16,
                               (s32)(h / 8u) - 16))
        return 1;
    kinfo("nv-chan", "VISIBLE 2D small-damage probe: CAB5 uploaded 32x32 at 128-byte pitch");
    /* submit_and_wait's release semaphore orders the CE writes.  The NVKMS
     * verifier below this layer reads through MemUtils, so host RM needs no
     * client-forbidden FB_FLUSH here. */
    kinfo("nv-chan", "VISIBLE 2D frame complete: CAB5 fill/copy rendered %ux%u scanout",
          w, h);
    return 0;
}

int nv_chan_visible_3d_hw(void) {
    if (!nv_chan_display_ready() || !g_compute_scanout_bound ||
        g_runtime_scanout_width < 320u || g_runtime_scanout_height < 240u)
        return -1;

    /* Clear with two full-screen SM-rasterized triangles, then draw three
     * foreground triangles.  Keeping clear and draw on CH_GFX removes the CE
     * cross-engine dependency which prevented the old visible path from ever
     * reaching its first compute dispatch after the observation dwell.
     * The last triangle is opaque magenta and
     * covers the exact center pixel, giving the KAPI client a deterministic
     * readback value (0xffff00ff) on every head.  Values are IEEE-754 bit
     * patterns because the kernel deliberately executes without an FPU ABI. */
    static const u32 scene[5 * 3 * 8] = {
        /* background quad / black (two clockwise triangles) */
        0xbf800000u,0xbf800000u,0,0x3f800000u, 0,0,0,0x3f800000u,
        0x3f800000u,0xbf800000u,0,0x3f800000u, 0,0,0,0x3f800000u,
        0xbf800000u,0x3f800000u,0,0x3f800000u, 0,0,0,0x3f800000u,
        0x3f800000u,0xbf800000u,0,0x3f800000u, 0,0,0,0x3f800000u,
        0x3f800000u,0x3f800000u,0,0x3f800000u, 0,0,0,0x3f800000u,
        0xbf800000u,0x3f800000u,0,0x3f800000u, 0,0,0,0x3f800000u,
        /* left / orange */
        0xbf800000u,0xbf800000u,0,0x3f800000u, 0x3f800000u,0x3f000000u,0,0x3f800000u,
        0,          0xbf800000u,0,0x3f800000u, 0x3f800000u,0x3f000000u,0,0x3f800000u,
        0xbf000000u,0x3f800000u,0,0x3f800000u, 0x3f800000u,0x3f000000u,0,0x3f800000u,
        /* right / green */
        0,          0xbf800000u,0,0x3f800000u, 0,0x3f800000u,0x3e800000u,0x3f800000u,
        0x3f800000u,0xbf800000u,0,0x3f800000u, 0,0x3f800000u,0x3e800000u,0x3f800000u,
        0x3f000000u,0x3f800000u,0,0x3f800000u, 0,0x3f800000u,0x3e800000u,0x3f800000u,
        /* center / magenta, drawn last */
        0xbf000000u,0xbf000000u,0,0x3f800000u, 0x3f800000u,0,0x3f800000u,0x3f800000u,
        0x3f000000u,0xbf000000u,0,0x3f800000u, 0x3f800000u,0,0x3f800000u,0x3f800000u,
        0,          0x3f400000u,0,0x3f800000u, 0x3f800000u,0,0x3f800000u,0x3f800000u,
    };
    int drawn = nv_chan_draw_triangles((const float *)scene, 5);
    if (drawn != 5) {
        kwarn("nv-chan", "VISIBLE 3D scene stopped after %d/5 SM-raster triangles", drawn);
        return 1;
    }
    /* WAIT_FOR_IDLE + the release semaphore orders the SM stores.  NVKMS then
     * validates this exact scanout allocation through MemUtils. */
    kinfo("nv-chan", "VISIBLE 3D scene complete: sm_120 rasterizer rendered a two-triangle clear plus three foreground triangles at %ux%u",
          g_runtime_scanout_width, g_runtime_scanout_height);
    return 0;
}

/* -------------------------------------------------------- NVENC H.264 encode
 * CFB7 inherits the C9B7 host method layout.  The 595.99.02 Blackwell codec
 * library contains the CFB7 ABI magic 0xCFB70006; NVIDIA's open-gpu-doc gives
 * the corresponding picture/status records in nvenc_drv.h.  This self-test
 * encodes one real 256x256 NV12 IDR and accepts it only when the card returns a
 * clean, bounded status plus an IDR NAL in the bitstream buffer.  This is an
 * encoder smoke test, not yet an encode/decode round-trip conformance test. */
#define SUBCH_NVENC                 4u
/* NV906F SET_OBJECT carries both the 16-bit class and an engine-routing field.
 * Mesa's video queue setup explicitly selects ENGINE_SW (0x1f in bits 20:16)
 * for Falcon video classes.  A class-only value leaves that field zero: PBDMA
 * can consume the ring, but the Falcon never receives a usable class context
 * and the channel ends in RC exceptType 109. */
#define NV906F_SET_OBJECT_ENGINE_SW (0x1fu << 16)
#define NVCFB7_SET_APPLICATION_ID   0x0200u
#define NVCFB7_SEMAPHORE_A          0x0240u
#define NVCFB7_SEMAPHORE_D          0x0304u
#define NVCFB7_EXECUTE              0x0300u
#define NVCFB7_SET_CONTROL_PARAMS   0x0700u
#define NVCFB7_SET_PICTURE_INDEX    0x0704u
#define NVCFB7_SET_IN_DRV_PIC_SETUP 0x0710u
#define NVCFB7_SET_IN_RC_DATA       0x070cu
#define NVCFB7_SET_IO_RC_PROCESS    0x0724u
#define NVCFB7_SET_OUT_ENC_STATUS   0x0718u
#define NVCFB7_SET_OUT_BITSTREAM    0x071cu
#define NVCFB7_SET_IOHISTORY        0x0720u
#define NVCFB7_SET_IN_COLOC_DATA    0x0728u
#define NVCFB7_SET_OUT_COLOC_DATA   0x072cu
#define NVCFB7_SET_OUT_REF_PIC_LUMA 0x0730u
#define NVCFB7_SET_IN_CUR_PIC       0x0734u
#define NVCFB7_SET_IN_CUR_PIC_CHROMA_U 0x0740u
#define NVCFB7_SET_IN_CUR_PIC_CHROMA_V 0x0744u
#define NVCFB7_SET_OUT_REF_PIC_CHROMA 0x074cu

#define VA_NVENC        0x00800000000ull
#define VA_NVENC_TILED  0x00880000000ull
#define H_NVENC_VRAM    0x004C0000u
#define NVENC_VRAM_BYTES 0xc0000u
#define NV_VIDEO_PTE_KIND 0x06u /* GB202 generic image memory, Mesa NIL */
static u64  g_nvenc_vram_fb;
static bool g_nvenc_vram_ready, g_nvenc_vram_attempted;
static bool g_nvenc_transport_ready;
static nvenc_h264_test_output_s g_nvenc_output;
static nvenc_cfb7_h264_drv_pic_setup_s g_nvenc_output_context;

/* Blackwell buffer-address methods use a two-word NON_INCREMENTING packet,
 * including a zero upper word for addresses below 1 TiB. Linux 595.99.02
 * libnvcuvid ELF 0x48ee0 and Windows 610.62 nvcuvid VA 0x1800085d0 both
 * select this form whenever channel class >0xc86f (ours is 0xca6f).
 * These are address bits 39:8 then 63:40, not adjacent method registers.
 * Keep ordinary controls and the separate semaphore A/B/C protocol unchanged.
 * See docs/nvenc-windows-cross-audit-20260917.md for the evidence boundary. */
static void nvenc_address(nv_channel_t *ch, u32 method, u64 address) {
    pb_data(ch, (3u << 29) | (2u << 16) | (SUBCH_NVENC << 13) |
                ((method >> 2) & 0xfffu));
    pb_data(ch, (u32)(address >> 8));
    pb_data(ch, (u32)(address >> 40));
}

/* Borrowed kernel-only snapshot, valid until the next encode attempt. Failed
 * attempts publish no output; callers must not retain it across another encode.
 * Readiness proves bounded retired output, not successful decoding. */
bool nv_nvenc_test_bitstream(const u8 **data, u32 *bytes, u32 *slice_start, u32 *slice_end) {
    if (!data || !bytes || !slice_start || !slice_end) return false;
    *data = NULL; *bytes = *slice_start = *slice_end = 0;
    if (!g_nvenc_output.bytes) return false;
    *data = g_nvenc_output.data; *bytes = g_nvenc_output.bytes;
    *slice_start = g_nvenc_output.slice_start; *slice_end = g_nvenc_output.slice_end;
    return true;
}

/* A self-contained stream from the SAME retired picture and configuration.
 * Caller owns private staging and serializes against the next encode attempt.
 * Metadata generation neither substitutes a fixture nor proves codec execution. */
static bool nv_nvenc_test_stream(u8 *out, u32 capacity, u32 *bytes,
                                 kh264_decode_desc *desc) {
    if (bytes) *bytes=0;
    if (desc) *desc=(kh264_decode_desc){0};
    u32 first=0,end=0;
    if (!g_nvenc_output.bytes || g_nvenc_output.bytes>sizeof g_nvenc_output.data ||
        !nvenc_h264_single_idr(g_nvenc_output.data,g_nvenc_output.bytes,&first,&end) ||
        first!=g_nvenc_output.slice_start || end!=g_nvenc_output.slice_end)
        return false;
    return nvenc_h264_make_stream(out,capacity,&g_nvenc_output_context,
                                  g_nvenc_output.data,g_nvenc_output.bytes,bytes,desc);
}

static bool ensure_nvenc_vram(nv_channel_t *ch) {
    if (g_nvenc_vram_ready) return true;
    /* RM/VMM failure can leave an allocated or partially mapped object. Do
     * not allocate again under its fixed handle without verified teardown. */
    if (g_nvenc_vram_attempted) return false;
    g_nvenc_vram_attempted = true;
    if (!nv_vram_alloc(ch->card, ch->rm, H_NVENC_VRAM,
                       NVENC_VRAM_BYTES, &g_nvenc_vram_fb)) {
        kwarn("nv-chan", "NVENC: no VRAM for encoder inputs/work/output");
        return false;
    }
    if (!nv_vmm_map(&ch->vmm, VA_NVENC, g_nvenc_vram_fb,
                    NVENC_VRAM_BYTES, true, false, false)) {
        kwarn("nv-chan", "NVENC: could not map encoder VRAM");
        return false;
    }
    /* GB202 images use GENERIC_MEMORY (kind 6), while the firmware records,
     * status and bitstream are ordinary kind-0 buffers.  Alias the allocation
     * with the image PTE kind and use that VA only for picture surfaces. */
    if (!nv_vmm_map_kind(&ch->vmm, VA_NVENC_TILED, g_nvenc_vram_fb,
                         NVENC_VRAM_BYTES, true, false, false,
                         NV_VIDEO_PTE_KIND)) {
        kwarn("nv-chan", "NVENC: could not map tiled encoder surfaces");
        return false;
    }
    if (!nv_vmm_commit(ch->card, ch->rm, &ch->vmm,
                       h_vaspace(CH_NVENC), "NVENC work buffer")) {
        kwarn("nv-chan", "NVENC: coherent VMM commit failed");
        return false;
    }
    g_nvenc_vram_ready = true;
    kinfo("nv-chan", "NVENC: %#x bytes at FB %#llx mapped to VA %#llx",
          NVENC_VRAM_BYTES, (unsigned long long)g_nvenc_vram_fb,
          (unsigned long long)VA_NVENC);
    return true;
}

/* Isolate class/application setup and the firmware's standalone flushing
 * release from picture processing. NVIDIA 595 ELF 0x22fc0/0x30590 emits
 * SEMAPHORE_D=0 as the release itself; it is not an EXECUTE parameter. This
 * gate proves only that the class can retire that operation, never encoding.
 * Keep the channel's normal timeout quarantine and do not retry a failed gate.
 * The channel and its semaphore allocation are boot-lifetime objects. */
static bool nvenc_transport_gate(nv_channel_t *ch) {
    if (ch->submit_failed) return false;
    if (g_nvenc_transport_ready) return true;
    if (!pb_reserve(ch, 64)) return false;
    ch->sem[0] = 0; cache_flush((const void *)&ch->sem[0], 4);
    u32 start = ch->pb_at;
    u32 signal = next_completion_signal(ch, 0x45430000u);
    pb_method(ch, SUBCH_NVENC, 0x0000u, 1);
        pb_data(ch, NVCFB7_VIDEO_ENCODER | NV906F_SET_OBJECT_ENGINE_SW);
    pb_method(ch, SUBCH_NVENC, NVCFB7_SET_APPLICATION_ID, 1); pb_data(ch, 1u);
    pb_method(ch, SUBCH_NVENC, NVCFB7_SEMAPHORE_A, 3);
        pb_data(ch, (u32)(VA_SEM >> 32)); pb_data(ch, (u32)VA_SEM); pb_data(ch, signal);
    pb_method(ch, SUBCH_NVENC, NVCFB7_SEMAPHORE_D, 1);
        pb_data(ch, nv_completion_awaken(ch, VIDEO_COMPLETION_AWAKEN));
    if (!submit_and_wait(ch, start, signal)) {
        kwarn("nv-chan", "NVENC transport gate failed before picture upload/EXECUTE; encode not attempted");
        return false;
    }
    g_nvenc_transport_ready = true;
    kinfo("nv-chan", "NVENC transport gate PASS: class/application release retired (encoding still unproven)");
    return true;
}

static void nvenc_surface(nvenc_h264_surface_cfg_s *s, u32 width, u32 height) {
    memset(s, 0, sizeof *s);
    s->frame_width_minus1 = (u16)(width - 1u);
    s->frame_height_minus1 = (u16)(height - 1u);
    s->sfc_pitch = 256;
    s->sfc_pitch_chroma = 256;
    /* Match the supplied Blackwell producer's block-linear reference/output
     * descriptor, not the older header's tiled-16x16-only reference comment.
     * Input uses the same two-GOB block height. The boot fixture is neutral;
     * application input is explicitly tiled with the independent Y/UV layouts.
     * Both planes/pitches are padded and kind-6 mapped. */
    s->block_height = NVENC_CFB7_TEST_BLOCK_HEIGHT;
    s->memory_mode = 0;                         /* NV12 semi-planar */
}

/* Shared boot/application picture path. Input is an immutable, caller-owned
 * kernel snapshot. NULL is reserved for the internal neutral boot fixture. */
#include "nvenc_rc_journal.h"
#include "nvenc_timeout_status.h"

static int nvenc_encode_frame_hw(const u8 *input, u32 input_bytes,
                                 kvideo_request_t *info) {
    g_nvenc_output.bytes = g_nvenc_output.slice_start = g_nvenc_output.slice_end = 0;
    if ((input && input_bytes != KVIDEO_ENCODE_INPUT_BYTES) || (!input && input_bytes))
        return -1;
    nv_channel_t *ch = &channels[CH_NVENC];
    if (!ch->open) { kwarn("nv-chan", "no NVENC channel to encode on"); return -1; }
    if (ch->submit_failed) return 1;
    if (info) info->phase=KVIDEO_PHASE_ALLOCATE;
    if (!ensure_nvenc_vram(ch)) return -1;
    if (info) info->phase=KVIDEO_PHASE_SUBMIT;
    if (!nvenc_transport_gate(ch)) return 1;

    enum {
        CFG=0x1000, SLICE=0x1300, ME=0x1400, MD=0x1500, QUANT=0x1600,
        WP=0x1700, STATUS=0x3000, STATUS_BYTES=0x8000,
        SLICE_STATS=0x100, MPEC_STATS=0x1100, FIFO_STATS=0x5100, AQ_STATS=0x5900,
        BITSTREAM=STATUS+STATUS_BYTES,
        ENC_WIDTH=NVENC_H264_TEST_WIDTH, ENC_HEIGHT=NVENC_H264_TEST_HEIGHT,
        /* IPCM carries 384 sample bytes per 4:2:0 macroblock, before headers
         * and emulation-prevention bytes. The old 64KiB output cannot contain
         * even the 96KiB sample payload. Reserve 256KiB and move every later
         * allocation with it; never enlarge only the firmware size field. */
        BITSTREAM_CAPACITY=0x40000,
        HISTORY=BITSTREAM+BITSTREAM_CAPACITY+0x4000,
        HISTORY_CAPACITY=0x18000,
        COLOC_IN=HISTORY+HISTORY_CAPACITY, COLOC_OUT=COLOC_IN+0x8000,
        IN_LUMA=COLOC_OUT+0x10000, IN_CHROMA=IN_LUMA+0x10000,
        OUT_LUMA=IN_CHROMA+0x10000, OUT_CHROMA=OUT_LUMA+0x10000,
        RC_STATS=OUT_CHROMA+0x8000, RC_STATS_CAPACITY=0x2000,
        RC_PROCESS=RC_STATS+RC_STATS_CAPACITY, RC_PROCESS_CAPACITY=0x3000,
        IPCM_TEST_BOUND=(ENC_WIDTH/16)*(ENC_HEIGHT/16)*(384+16)*3/2+4096
    };
    _Static_assert(BITSTREAM_CAPACITY >= IPCM_TEST_BOUND, "forced IPCM output capacity");
    _Static_assert(BITSTREAM_CAPACITY == sizeof g_nvenc_output.data, "full encoder output snapshot capacity");
    _Static_assert(OUT_CHROMA+0x8000 <= NVENC_VRAM_BYTES, "NVENC layout exceeds allocation");
    _Static_assert(RC_STATS+RC_STATS_CAPACITY <= NVENC_VRAM_BYTES, "NVENC RC stats exceed allocation");
    _Static_assert(RC_PROCESS_CAPACITY >= NVENC_CFB7_RC_PROCESS_BYTES, "NVENC RC work pool too small");
    _Static_assert(RC_PROCESS+RC_PROCESS_CAPACITY <= NVENC_VRAM_BYTES, "NVENC RC work pool exceeds allocation");
    /* NVIDIA's status buffer is not just nvenc_pic_stat_s. Slice output must
     * not alias that header; MPEC/FIFO/AQ storage must not alias bitstream data.
     * Reserve one record per MB (also covers the one-slice proof). 0x100 and
     * 0x1100 match the successful owned 256x256 CFB7 capture. ACT is relative
     * to the separately bound RC statistics buffer, never the status buffer. */
    _Static_assert(sizeof(nvenc_pic_stat_s) <= SLICE_STATS, "NVENC picture/slice stats overlap");
    _Static_assert(SLICE_STATS + (ENC_WIDTH/16)*(ENC_HEIGHT/16)*sizeof(nvenc_slice_stat_s) <= MPEC_STATS, "NVENC slice/MPEC stats overlap");
    _Static_assert(MPEC_STATS + (ENC_WIDTH/16)*(ENC_HEIGHT/16)*sizeof(nvenc_mpec_stat_s) <= FIFO_STATS, "NVENC MPEC/FIFO stats overlap");
    _Static_assert(FIFO_STATS + (ENC_WIDTH/16)*(ENC_HEIGHT/16)*sizeof(nvenc_stats_fifo_s) <= AQ_STATS, "NVENC FIFO/AQ stats overlap");
    _Static_assert(AQ_STATS + (ENC_WIDTH/16)*(ENC_HEIGHT/16)*sizeof(nvenc_aq_stat_s) <= STATUS_BYTES, "NVENC stats exceed allocation");
    /* Keep the existing placement until dynamic RM allocation is wired up,
     * but validate it against the actual CFB7 sizing contract. In particular,
     * coloc needs padded row pairs, not merely 64 bytes times the MB count. */
    nvenc_h264_buffer_requirements_s requirements;
    if (info) info->phase=KVIDEO_PHASE_LAYOUT;
    if (!nvenc_h264_buffer_requirements(&requirements,ENC_WIDTH,ENC_HEIGHT) ||
        requirements.pitch!=256u || requirements.history_bytes>HISTORY_CAPACITY ||
        requirements.history_allocation_bytes>HISTORY_CAPACITY ||
        requirements.coloc_bytes>0x8000u || requirements.luma_bytes>0x10000u ||
        requirements.chroma_bytes>0x8000u || requirements.status_bytes>STATUS_BYTES ||
        requirements.rc_stat_bytes>RC_STATS_CAPACITY ||
        requirements.slice_stat_offset!=SLICE_STATS || requirements.mpec_stat_offset!=MPEC_STATS ||
        requirements.fifo_stat_offset!=FIFO_STATS || requirements.aq_stat_offset!=AQ_STATS) {
        kerr("nv-chan", "NVENC: allocated layout does not cover CFB7 buffer requirements");
        return -1;
    }
    /* Build one complete CPU shadow, then coherently upload the RM allocation.
     * Besides avoiding dozens of host-RM-incoherent PRAMIN writes, this makes
     * every firmware-visible work buffer deterministically initialized.
     * Linux's history initializer (ELF c7a30 -> 1275b0 -> 2da00 -> 2d6a0)
     * explicitly CE-fills the logical history extent with zero. Preserve that
     * zero region here; the coherent upload must finish before EXECUTE. */
    static u8 stage[NVENC_VRAM_BYTES] __attribute__((aligned(256)));
    memset(stage, 0, sizeof stage);
    memset(stage + STATUS, 0xa5, sizeof(nvenc_pic_stat_s));
    memset(stage + IN_LUMA, 0x80, 0x10000);
    memset(stage + IN_CHROMA, 0x80, 0x8000);
    if (input) {
        nv_video_nv12_layout_s linear,tiled;
        if (info) info->phase=KVIDEO_PHASE_TILE;
        if (!nv_video_nv12_layout_init(&linear,NV_VIDEO_LINEAR_NV12,
                ENC_WIDTH,ENC_HEIGHT,0,0,0,0) ||
            !nv_video_nv12_layout_init(&tiled,NV_VIDEO_BLACKWELL_GOB2_NV12,
                ENC_WIDTH,ENC_HEIGHT,256,256,0,0) ||
            tiled.uv_offset!=IN_CHROMA-IN_LUMA ||
            tiled.bytes>OUT_LUMA-IN_LUMA ||
            !nv_video_nv12_convert(stage+IN_LUMA,OUT_LUMA-IN_LUMA,&tiled,
                                   input,input_bytes,&linear)) return -1;
    }
    u8 *cfg_blob = stage + CFG;
    nvenc_cfb7_h264_drv_pic_setup_s *cfg = (nvenc_cfb7_h264_drv_pic_setup_s *)cfg_blob;
    _Static_assert(sizeof(*cfg) <= SLICE-CFG, "CFB7 picture overlaps slice control");
    nvenc_h264_slice_control_s *sl = (nvenc_h264_slice_control_s *)(stage + SLICE);
    nvenc_h264_me_control_s *me = (nvenc_h264_me_control_s *)(stage + ME);
    nvenc_h264_md_control_s *md = (nvenc_h264_md_control_s *)(stage + MD);
    nvenc_h264_quant_control_s *q = (nvenc_h264_quant_control_s *)(stage + QUANT);
    nvenc_pred_weight_table_s *wp = (nvenc_pred_weight_table_s *)(stage + WP);
    _Static_assert(SLICE + sizeof(nvenc_h264_slice_control_s) <= ME, "NVENC slice overlaps ME");
    _Static_assert(ME + sizeof(nvenc_h264_me_control_s) <= MD, "NVENC ME overlaps MD");
    _Static_assert(MD + sizeof(nvenc_h264_md_control_s) <= QUANT, "NVENC MD overlaps quant");
    _Static_assert(QUANT + sizeof(nvenc_h264_quant_control_s) <= WP, "NVENC quant overlaps weights");
    _Static_assert(WP + sizeof(nvenc_pred_weight_table_s) <= STATUS, "NVENC weights overlap status");
    (void)wp;

    cfg->magic = NVENC_CFB7_DRV_MAGIC;
    nvenc_surface(&cfg->refpic_cfg, ENC_WIDTH, ENC_HEIGHT);
    nvenc_surface(&cfg->input_cfg, ENC_WIDTH, ENC_HEIGHT);
    nvenc_surface(&cfg->outputpic_cfg, ENC_WIDTH, ENC_HEIGHT);
    /* PTE kind and block height alone do not describe Blackwell byte order.
     * Tell firmware that every Y plane is R8 and every UV plane is R8G8.
     * Leaving the CFB7 extension zero selects the wrong image layout even
     * though these surfaces and their linear upload alias share local VRAM. */
    cfg->plane_layouts = NVENC_CFB7_GOB_NV12_LAYOUTS;
    cfg->sps_data.profile_idc = 66;             /* constrained baseline */
    cfg->sps_data.level_idc = 20;
    cfg->sps_data.chroma_format_idc = 1;
    cfg->sps_data.pic_order_cnt_type = 0;
    cfg->sps_data.frame_mbs_only = 1;
    cfg->pps_data.pic_init_qp_minus26 = 0;
    cfg->pps_data.deblocking_filter_control_present_flag = 1;
    if (!nvenc_cfb7_h264_cqp_idr_defaults(&cfg->rate_control,26,30)) return -1;
    /* This independent IDR has no previous pictures. Match the supplied
     * driver's missing-reference sentinel rather than naming DPB slot zero. */
    memset(cfg->pic_control.l0, NVENC_CFB7_H264_NO_REF, sizeof cfg->pic_control.l0);
    memset(cfg->pic_control.l1, NVENC_CFB7_H264_NO_REF, sizeof cfg->pic_control.l1);
    cfg->pic_control.slice_control_offset = SLICE-CFG;
    cfg->pic_control.me_control_offset = ME-CFG;
    cfg->pic_control.md_control_offset = MD-CFG;
    cfg->pic_control.q_control_offset = QUANT-CFG;
    cfg->pic_control.wp_control_offset = WP-CFG;
    /* Describe the driver's computed history extent, not the spare capacity
     * in our fixed backing allocation. CFB7's producer (ELF 0xd147d..0xd1499)
     * writes align_up(width_in_mbs * 384, 256): 6144 bytes for this frame.
     * Backing capacity separately covers Linux's 16-slot pool (96 KiB here).
     * Do not put the entire pool size in this per-slot firmware field. */
    cfg->pic_control.hist_buf_size = requirements.history_bytes;
    cfg->pic_control.bitstream_buf_size = BITSTREAM_CAPACITY;
    cfg->pic_control.slice_stat_offset = SLICE_STATS;
    cfg->pic_control.mpec_stat_offset = MPEC_STATS;
    cfg->pic_control.stats_fifo_offset = FIFO_STATS;
    cfg->pic_control.aq_stat_offset = AQ_STATS;
    cfg->pic_control.act_stat_offset = requirements.rc_act_offset;
    cfg->pic_control.pic_type = 3;               /* IDR */
    cfg->pic_control.ref_pic_flag = 1;
    cfg->pic_control.slice_mode = 1;             /* static one-slice array */
    cfg->pic_control.codec = NVENC_CFB7_PICTURE_CODEC_H264;
    /* Full-frame mode is not a one-strip submission. NVIDIA 595's picture
     * producer zeroes the record; ELF 0xde136..0xde17c writes strip ID/row
     * controls only when object+0x1359 indicates multiple strips. Preserve
     * those zero fields here, matching the full-frame CFB7 capture. The
     * slice-control array still covers all macroblocks via sl->num_mb below. */
    cfg->gpTimer_timeout_val = nvenc_cfb7_h264_timer(ENC_WIDTH, ENC_HEIGHT);
    /* The ME record's five hint types must cover 0..4, not five copies of
     * CONST from a zero-filled record. NVIDIA 595 (ELF 0xcfb97..0xcfba6)
     * writes 0x4688 to ME+0x9a: const/spatial/temporal/coloc/external.
     * This ordering alone does not enable any external-hint source. */
    me->hint_type0 = 0;
    me->hint_type1 = 1;
    me->hint_type2 = 2;
    me->hint_type3 = 3;
    me->hint_type4 = 4;
    sl->num_mb = (ENC_WIDTH / 16) * (ENC_HEIGHT / 16);
    sl->qp_avr = 26;
    sl->qp_slice_min = 0;
    sl->qp_slice_max = 51;
    sl->force_intra = 1;
    sl->limit_slice_top_boundary = sl->limit_slice_bot_boundary = 1;
    sl->limit_slice_left_boundary = sl->limit_slice_right_boundary = 1;
    /* Boot and applications use the same normal 16x16 intra policy at QP26.
     * Do not quarantine production encoding on a different, forced-IPCM-only
     * bootstrap path. The neutral frame still has an exact-byte round-trip
     * check: the independent Windows QP26 capture decodes to all 0x80 without
     * IPCM. This alignment is not proof of native firmware completion. */
    md->intra_luma16x16_mode_enable = 0xf;
    md->intra_chroma_mode_enable = 0xf;
    md->ip_search_mode = 4;
    md->force_ipcm = 0;
    /* NVIDIA's quant producer (ELF 0xd0470) always copies the initialized
     * 192-byte record. IPCM is an MD choice, not permission to give firmware
     * an uninitialized auxiliary record. Both boot and application input get
     * the same initialized defaults. Keep the existing worst-case capacity. */
    nvenc_h264_quant_defaults(q);
    /* Published mode 3 disables early intra shortcuts; the full intra
     * decision uses ip_search_mode's explicitly enabled 16x16 size. */
    md->early_intra_mode_control=3;

    /* Match the ordinary reference resource lifecycle, including the two
     * initialized RC records. Each call is a new independent-IDR stream; no
     * inter-frame/VBR state is advertised or carried across these calls. */
    if (!nvenc_h264_cqp_idr_work_init(stage+RC_PROCESS,RC_PROCESS_CAPACITY,
                                      &cfg->rate_control)) {
        kerr("nv-chan", "NVENC: invalid independent-IDR RC work initialization");
        return -1;
    }

    if (info) info->phase=KVIDEO_PHASE_UPLOAD;
    if (!nv_vram_object_write(ch, H_NVENC_VRAM, g_nvenc_vram_fb,
                              0, stage, sizeof stage)) {
        kerr("nv-chan", "NVENC: coherent firmware-buffer upload failed");
        return 1;
    }

    if (info) info->phase=KVIDEO_PHASE_SUBMIT;
    ch->sem[0] = 0; cache_flush((const void *)&ch->sem[0], 4);
    /* Thirteen 12-byte address packets plus 64 bytes of control/fence packets. */
    if (!pb_reserve(ch, 256)) return 1;
    u32 start = ch->pb_at;
    u32 signal = next_completion_signal(ch, 0x454e0000u);
    pb_method(ch, SUBCH_NVENC, 0x0000u, 1);
        pb_data(ch, NVCFB7_VIDEO_ENCODER | NV906F_SET_OBJECT_ENGINE_SW);
    pb_method(ch, SUBCH_NVENC, NVCFB7_SET_APPLICATION_ID, 1); pb_data(ch, 1u);
    pb_method(ch, SUBCH_NVENC, NVCFB7_SET_CONTROL_PARAMS, 1);
        /* NVIDIA 595 emitter (ELF 0x11ff80) sets FORCE_OUT_COL whenever
         * its output-coloc object is bound at 0x72c. We bind COLOC_OUT below.
         * H264 + force reconstructed/coloc output + GPTIMER; bit 4 TOPLEVEL
         * is not set by that H264 emitter (nor is it NVDEC's timer bit). */
        pb_data(ch, 3u | (1u << 8) | (1u << 9) | (1u << 12));
    pb_method(ch, SUBCH_NVENC, NVCFB7_SET_PICTURE_INDEX, 1); pb_data(ch, 0u);
    nvenc_address(ch, NVCFB7_SET_IN_DRV_PIC_SETUP, VA_NVENC + CFG);
    nvenc_address(ch, NVCFB7_SET_OUT_ENC_STATUS, VA_NVENC + STATUS);
    nvenc_address(ch, NVCFB7_SET_OUT_BITSTREAM, VA_NVENC + BITSTREAM);
    nvenc_address(ch, NVCFB7_SET_IOHISTORY, VA_NVENC + HISTORY);
    nvenc_address(ch, NVCFB7_SET_IN_COLOC_DATA, VA_NVENC + COLOC_IN);
    nvenc_address(ch, NVCFB7_SET_OUT_COLOC_DATA, VA_NVENC + COLOC_OUT);
    nvenc_address(ch, NVCFB7_SET_IO_RC_PROCESS, VA_NVENC + RC_PROCESS);
    nvenc_address(ch, NVCFB7_SET_OUT_REF_PIC_LUMA, VA_NVENC_TILED + OUT_LUMA);
    nvenc_address(ch, NVCFB7_SET_OUT_REF_PIC_CHROMA, VA_NVENC_TILED + OUT_CHROMA);
    nvenc_address(ch, NVCFB7_SET_IN_CUR_PIC, VA_NVENC_TILED + IN_LUMA);
    nvenc_address(ch, NVCFB7_SET_IN_CUR_PIC_CHROMA_U, VA_NVENC_TILED + IN_CHROMA);
    nvenc_address(ch, NVCFB7_SET_IN_CUR_PIC_CHROMA_V, VA_NVENC_TILED + IN_CHROMA);
    /* First-pass stats output, despite the method's historical SET_IN name.
     * Separate from the initialized persistent work state at 0x724 above.
     * Picture-level VBR/CBR remains disabled. */
    nvenc_address(ch, NVCFB7_SET_IN_RC_DATA, VA_NVENC + RC_STATS);
    /* SEMAPHORE_D is a standalone RELEASE, not configuration for EXECUTE.
     * NVIDIA 595 libnvcuvid FUN_00122fc0/FUN_00130590 emit it as the fence
     * operation itself.  Launch first, then enqueue the flushing release;
     * otherwise a healthy engine can signal before writing status/output. */
    pb_method(ch, SUBCH_NVENC, NVCFB7_EXECUTE, 1); pb_data(ch, 0u);
    pb_method(ch, SUBCH_NVENC, NVCFB7_SEMAPHORE_A, 3);
        pb_data(ch, (u32)(VA_SEM >> 32)); pb_data(ch, (u32)VA_SEM); pb_data(ch, signal);
    pb_method(ch, SUBCH_NVENC, NVCFB7_SEMAPHORE_D, 1);
        pb_data(ch, nv_completion_awaken(ch, VIDEO_COMPLETION_AWAKEN));

    kinfo("nv-chan", "NVENC: encoding a %ux%u NV12 IDR (CFB7 ABI magic %#x, record=%u timer=%#x capacity=%u)",
          (u32)ENC_WIDTH, (u32)ENC_HEIGHT, NVENC_CFB7_DRV_MAGIC,
          (u32)sizeof(*cfg), cfg->gpTimer_timeout_val, (u32)BITSTREAM_CAPACITY);
    kinfo("nv-chan", "NVENC: block-linear input/reference/output block-height=%u pitch=%u/%u image-kind=%#x plane-layouts=%#x",
          (u32)cfg->input_cfg.block_height, (u32)cfg->input_cfg.sfc_pitch,
          (u32)cfg->input_cfg.sfc_pitch_chroma, (u32)NV_VIDEO_PTE_KIND,
          cfg->plane_layouts);
    kinfo("nv-chan", "NVENC: firmware-pass=%u RC stats=%u/%u activity-offset=%u; CQP, no picture RC",
          (u32)cfg->rate_control.two_pass_rc, requirements.rc_stat_bytes,
          (u32)RC_STATS_CAPACITY, requirements.rc_act_offset);
    bool signalled = submit_and_wait(ch, start, signal);
    if (!signalled) {
        kwarn("nv-chan", "NVENC did not retire; retaining its buffers, not interpreting unwritten status/bitstream as output");
        nvenc_capture_rc_journal(ch);
        nvenc_capture_unretired_status(ch, STATUS);
        return 1;
    }

    if (info) info->phase=KVIDEO_PHASE_STATUS;
    nvenc_pic_stat_s st;
    if (!nv_vram_object_read(ch, H_NVENC_VRAM, g_nvenc_vram_fb,
                             STATUS, &st, sizeof st)) {
        kerr("nv-chan", "NVENC: coherent status readback failed");
        return 1;
    }
    if (info) {
        /* Preserve the API's zero-on-success convention. The low two bits
         * are a completion state, despite the legacy ABI member's name. */
        info->firmware_error=st.error_status == NVENC_CFB7_H264_STATUS_COMPLETE
            ? 0u : st.error_status + 1u;
        info->slice_error=st.ucode_error_status;
    }
    /* Completed H.264 output uses the byte-aligned total bit count. The
     * partial-output cursor is not a completed-picture length (Linux reader
     * ELF 0xbbd30, Windows reader 0x180102fe0). Never extend output from it. */
    if (st.error_status != NVENC_CFB7_H264_STATUS_COMPLETE || st.ucode_error_status != 0u ||
        st.total_bit_count == 0u || st.total_bit_count > BITSTREAM_CAPACITY * 8u ||
        (st.total_bit_count & 7u) != 0u) {
        kwarn("nv-chan", "NVENC: rejecting picture state=%u (complete=2) ucode=%#x bits=%u last-byte=%u",
              st.error_status, st.ucode_error_status, st.total_bit_count,
              st.last_valid_byte_offset);
        nvenc_capture_rc_journal(ch);
        return 1;
    }
    /* The published status echoes SetPictureIndex and pic_control.pic_type.
     * This test requests exactly one IDR slice at buffer offset zero. Reject
     * a different completion before consuming or publishing its bytes. */
    if (st.picture_index != 0u || st.pic_type != cfg->pic_control.pic_type ||
        st.num_slices != 1u ||
        st.bitstream_start_pos != cfg->pic_control.bitstream_start_pos) {
        kwarn("nv-chan", "NVENC: mismatched completion picture=%u type=%u slices=%u start-byte=%u",
              st.picture_index, (u32)st.pic_type, (u32)st.num_slices,
              st.bitstream_start_pos);
        return 1;
    }
    u32 bytes = st.total_bit_count / 8u;
    if (info) info->phase=KVIDEO_PHASE_READBACK;
    if (!nv_vram_object_read(ch, H_NVENC_VRAM, g_nvenc_vram_fb,
                              BITSTREAM, g_nvenc_output.data, (bytes + 3u) & ~3u)) {
        kerr("nv-chan", "NVENC: coherent bitstream readback failed");
        return 1;
    }
    u32 slice_start = 0, slice_end = 0;
    bool annexb = nvenc_h264_single_idr(g_nvenc_output.data, bytes, &slice_start, &slice_end);
    kinfo("nv-chan", "NVENC: sem=%s picture state=%u ucode=%#x bits=%u bytes=%u nal=%u",
          signalled ? "signalled" : "silent", st.error_status,
          st.ucode_error_status, st.total_bit_count, bytes, annexb ? 5u : 0u);
    if (st.error_status == NVENC_CFB7_H264_STATUS_COMPLETE && st.ucode_error_status == 0u && bytes && annexb) {
        g_nvenc_output_context = *cfg;
        g_nvenc_output.slice_start = slice_start; g_nvenc_output.slice_end = slice_end;
        g_nvenc_output.bytes = bytes; /* publish only after full readback and framing checks */
        kinfo("nv-chan", "NVENC produced bounded Annex-B IDR output on the real card (round-trip validation pending)");
        return 0;
    }
    kwarn("nv-chan", "NVENC encode unproven: no clean non-empty Annex-B output");
    return 1;
}

int nv_nvenc_selftest_hw(void) {
    return nvenc_encode_frame_hw(NULL,0,NULL);
}

/* Applications use the same native command path as boot, never a fixture's
 * compressed bytes. The render transaction serializes the persistent encoder
 * work buffers and the short-lived global output snapshot. Only the internal
 * boot wrapper may request the neutral synthetic input. */
int nv_nvenc_encode_idr(const unsigned char *input,unsigned int bytes,
                       unsigned char *output,unsigned int capacity,
                       kvideo_request_t *info) {
    if (!info || info->version!=KVIDEO_ABI || info->reserved ||
        (info->operation!=KVIDEO_H264_ENCODE_PLAN &&
         info->operation!=KVIDEO_H264_ENCODE_IDR)) return -E_INVAL;
    u32 width=info->coded_width,height=info->coded_height;
    info->display_width=info->display_height=0;
    info->crop_left=info->crop_top=info->crop_right=info->crop_bottom=0;
    info->pitch=info->required_bytes=info->written_bytes=0;
    info->parse_status=info->firmware_error=info->slice_error=0;
    info->decoded_mbs=info->error_mbs=0;
    info->phase=KVIDEO_PHASE_LAYOUT;
    /* Reject unsupported geometry; do not silently resize the caller's frame
     * or scale a firmware work-buffer requirement without driver evidence. */
    if (width!=KVIDEO_ENCODE_WIDTH || height!=KVIDEO_ENCODE_HEIGHT) {
        info->parse_status=KH264_UNSUPPORTED; return -E_INVAL;
    }
    _Static_assert(KVIDEO_ENCODE_WIDTH==NVENC_H264_TEST_WIDTH &&
                   KVIDEO_ENCODE_HEIGHT==NVENC_H264_TEST_HEIGHT,
                   "application and native encoder geometry disagree");
    _Static_assert(KVIDEO_ENCODE_OUTPUT_MAX==NVENC_H264_STREAM_CAPACITY,
                   "application and native encoder output bounds disagree");
    info->display_width=width; info->display_height=height; info->pitch=width;
    info->required_bytes=KVIDEO_ENCODE_OUTPUT_MAX;
    if (info->operation==KVIDEO_H264_ENCODE_PLAN) {
        info->phase=KVIDEO_PHASE_COMPLETE; return 0;
    }
    if (!input || bytes!=KVIDEO_ENCODE_INPUT_BYTES || !output ||
        capacity<info->required_bytes) return -E_INVAL;
    if (!channels[CH_NVENC].open) return -E_NOSYS;
    if (channels[CH_NVENC].submit_failed) return -E_IO;
    int rc=nvenc_encode_frame_hw(input,bytes,info);
    if (rc) return -E_IO;
    info->phase=KVIDEO_PHASE_PACKAGE;
    kh264_decode_desc desc;
    u32 written=0;
    bool packaged=nv_nvenc_test_stream(output,capacity,&written,&desc);
    /* Retire the borrowed snapshot before releasing the shared transaction.
     * It must not be exposed later as another process's boot-test output. */
    g_nvenc_output.bytes=g_nvenc_output.slice_start=g_nvenc_output.slice_end=0;
    if (!packaged || !written || written>info->required_bytes ||
        desc.coded_width!=width || desc.coded_height!=height) return -E_IO;
    info->written_bytes=written; info->phase=KVIDEO_PHASE_COMPLETE;
    return 0;
}

/* ------------------------------------------------------- NVDEC H.264 decode */
/* Video decode binds to subchannel 4 (Mesa nv_push.h: SUBC_NVC5B0=4).  On the
 * wrong subchannel the falcon cannot establish the class context for the pushed
 * methods, which manifests as a CTX_SWITCH_TIMEOUT (Xid 109) - the same class of
 * bug the compute subchannel-0 mismatch was (Xid 13). */
#define SUBCH_NVDEC 4


/* NVDEC engine-read buffers (pic-setup, bitstream, slice/coloc/history/mbhist,
 * status, and the output surfaces) must be VRAM-resident - the falcon + decode
 * engine read them through the GPU MMU, not a PCIe DMA, so a sysmem aperture
 * faults the same way compute's sysmem QMD did.  NVK places every NVDEC buffer in
 * device-local VRAM.  Stage in the sysmem pushbuffer, copy to this VRAM buffer,
 * hand the engine the VRAM VAs, and read the output back from VRAM. */
#define VA_NVDEC        0x00600000000ull
#define VA_NVDEC_TILED  0x00680000000ull
#define H_NVDEC_VRAM    0x004B0000u
#define NVDEC_VRAM_BYTES 0x20000u
static u64  g_nvdec_vram_fb;
static bool g_nvdec_vram_ready, g_nvdec_vram_attempted;

static bool ensure_nvdec_vram(nv_channel_t *ch) {
    if (g_nvdec_vram_ready) return true;
    /* As for NVENC, retain incomplete setup rather
     * than retrying allocation over a live RM handle or partial mapping. */
    if (g_nvdec_vram_attempted) return false;
    g_nvdec_vram_attempted = true;
    if (!nv_vram_alloc(ch->card, ch->rm, H_NVDEC_VRAM,
                       NVDEC_VRAM_BYTES, &g_nvdec_vram_fb)) {
        kwarn("nv-chan", "NVDEC: no VRAM for the decode buffers"); return false;
    }
    if (!nv_vmm_map(&ch->vmm, VA_NVDEC, g_nvdec_vram_fb,
                    NVDEC_VRAM_BYTES, true, false, false)) {
        kwarn("nv-chan", "NVDEC: could not map the VRAM decode buffers"); return false;
    }
    if (!nv_vmm_map_kind(&ch->vmm, VA_NVDEC_TILED, g_nvdec_vram_fb,
                         NVDEC_VRAM_BYTES, true, false, false,
                         NV_VIDEO_PTE_KIND)) {
        kwarn("nv-chan", "NVDEC: could not map tiled output surfaces"); return false;
    }
    if (!nv_vmm_commit(ch->card, ch->rm, &ch->vmm,
                       h_vaspace(CH_NVDEC), "NVDEC work buffer")) {
        kwarn("nv-chan", "NVDEC: coherent VMM commit failed");
        return false;
    }
    kinfo("nv-chan", "NVDEC: decode VRAM buffer at fb %#llx mapped at VA %#llx",
          (unsigned long long)g_nvdec_vram_fb, (unsigned long long)VA_NVDEC);
    g_nvdec_vram_ready = true;
    return true;
}

/* Decode an above-minimum 64x64 I_PCM IDR on the real NVDEC engine and check the
 * complete NV12 frame and firmware status. Returns 0 iff the retired decode
 * produced the expected pixels without reported errors. Self-guards on the channel being
 * open; never touches the proven copy/compute paths. */
int nv_nvdec_selftest_hw(void) {
    nv_channel_t *ch = &channels[CH_NVDEC];
    if (!ch->open) { kwarn("nv-chan", "no NVDEC channel to decode on"); return -1; }
    if (ch->submit_failed) return 1;

    /* Data staging below 64 KiB; methods live in the disjoint upper ring. */
    const u32 in_off=0x1000, pic_off=0x3000, slc_off=0x3400, col_off=0x3800,
              his_off=0x4000, mbh_off=0x5000, sts_off=0x6000, lum_off=0x8000,
              chr_off=0xa000;
    /* GOB_2 is 64 bytes wide and two 8-line GOBs high. */
    const u32 luma_pitch=64, chroma_pitch=64, hist_size=0xc00,
              coloc_size=0x800, mbhist_size=0x200;
    volatile u8 *base = ch->pushbuf;

    /* Read actual stream parameters, not a parallel hard-coded SPS/PPS copy.
     * Keep this known-good fixture's allocation/pixel oracle fixed; arbitrary
     * dimensions need their own checked allocation and output conversion. */
    static nvdec_h264_pic_s pic;
    kh264_decode_desc desc;
    int parse_rc = nvdec_h264_parse_pic(&pic, &desc,
        h264_iframe_test, H264_TEST_LEN, luma_pitch, chroma_pitch,
        hist_size, mbhist_size);
    if (parse_rc != KH264_OK || desc.coded_width != H264_TEST_WIDTH ||
        desc.coded_height != H264_TEST_HEIGHT ||
        desc.slice_start != H264_TEST_SLICE_OFFSET || desc.slice_end != H264_TEST_LEN) {
        kerr("nv-chan", "NVDEC: fixture headers/layout rejected before upload (parse=%d)", parse_rc);
        return 1;
    }

    /* bitstream + 16-byte EOS */
    for (u32 i = 0; i < H264_TEST_LEN; i++) base[in_off + i] = h264_iframe_test[i];
    static const unsigned char EOS[16] = {0,0,1,0xb,0,0,0,0,0,0,1,0xb,0,0,0,0};
    for (u32 i = 0; i < 16; i++) base[in_off + H264_TEST_LEN + i] = EOS[i];
    u32 stream_len = H264_TEST_LEN + 16;

    /* slice offsets: [start of slice NAL, total length] */
    *(volatile u32 *)(base + slc_off + 0) = desc.slice_start;
    *(volatile u32 *)(base + slc_off + 4) = desc.slice_end;

    /* zero the working buffers, fill the pic setup */
    for (u32 i = 0; i < 0x400; i++) base[pic_off + i] = 0;
    for (u32 i = 0; i < hist_size; i++) base[his_off + i] = 0;
    for (u32 i = 0; i < coloc_size; i++) base[col_off + i] = 0;
    for (u32 i = 0; i < mbhist_size; i++) base[mbh_off + i] = 0;
    for (u32 i = 0; i < 4096; i++) base[sts_off + i] = 0xff; /* unwritten status must fail */
    for (u32 i = 0; i < 0x2000; i++) base[lum_off + i] = 0; /* pre-clear output */
    for (u32 i = 0; i < 0x2000; i++) base[chr_off + i] = 0;
    for (u32 i = 0; i < sizeof pic; i++) base[pic_off + i] = ((const u8 *)&pic)[i];

    cache_flush(base + in_off, stream_len);
    cache_flush(base + pic_off, sizeof pic);
    cache_flush(base + slc_off, 8);
    cache_flush(base + lum_off, 4096);

    /* Copy the whole staged data region into the RM-owned allocation through
     * MemUtils on host RM.  The method stream itself remains in coherent
     * sysmem for PBDMA. */
    if (!ensure_nvdec_vram(ch)) return -1;
    if (!nv_vram_object_write(ch, H_NVDEC_VRAM, g_nvdec_vram_fb,
                              in_off, (const void *)(base + in_off),
                              chr_off + 0x2000u - in_off)) {
        kerr("nv-chan", "NVDEC: coherent decode-buffer upload failed");
        return 1;
    }

    #define VAP(off) ((u32)((VA_NVDEC + (off)) >> 8))
    #define VAS(off) ((u32)((VA_NVDEC_TILED + (off)) >> 8))
    if (!pb_reserve(ch, 192u)) return 1;
    u32 start = ch->pb_at;
    /* Allocating the engine object under the channel does not select it on a
     * PBDMA subchannel.  Copy and compute both begin every stream with method
     * zero/SET_OBJECT; NVDEC used to omit it and immediately sent CFB0 methods
     * to an unbound subchannel, after which the ring was consumed but the
     * falcon context switch timed out (RC exceptType 109). */
    pb_method(ch, SUBCH_NVDEC, 0x0000u, 1);
        pb_data(ch, NVCFB0_VIDEO_DECODER | NV906F_SET_OBJECT_ENGINE_SW);
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_APPLICATION_ID, 1);   pb_data(ch, NVCFB0_SET_APPLICATION_ID_ID_H264);
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_CONTROL_PARAMS, 1);   pb_data(ch, NVCFB0_H264_CONTROL_PARAMS);
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_DRV_PIC_SETUP_OFFSET, 1);  pb_data(ch, VAP(pic_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_IN_BUF_BASE_OFFSET, 1);    pb_data(ch, VAP(in_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_PICTURE_INDEX, 1);         pb_data(ch, 0);
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_SLICE_OFFSETS_BUF_OFFSET, 1); pb_data(ch, VAP(slc_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_COLOC_DATA_OFFSET, 1);     pb_data(ch, VAP(col_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_HISTORY_OFFSET, 1);        pb_data(ch, VAP(his_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_NVDEC_STATUS_OFFSET, 1);   pb_data(ch, VAP(sts_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_PICTURE_LUMA_OFFSET0, 1);  pb_data(ch, VAS(lum_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SET_PICTURE_CHROMA_OFFSET0, 1);pb_data(ch, VAS(chr_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_H264_SET_MBHIST_BUF_OFFSET, 1);pb_data(ch, VAP(mbh_off));
    pb_method(ch, SUBCH_NVDEC, NVCFB0_EXECUTE, 1);                   pb_data(ch, NVCFB0_EXECUTE_VALUE);
    /* A C5B0/CFB0 backend semaphore is ordered after decode by its default
     * flush behavior.  This is the same completion primitive current Mesa/NVK
     * emits for video queues and, unlike host-owned RAMFC GP_GET, is readable
     * by Kestrel on the host-RM path. */
    u32 signal = next_completion_signal(ch, 0x44450000u);
    ch->sem[0] = 0; cache_flush((const void *)&ch->sem[0], 4);
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SEMAPHORE_A, 3);
        pb_data(ch, (u32)(VA_SEM >> 32)); pb_data(ch, (u32)VA_SEM); pb_data(ch, signal);
    pb_method(ch, SUBCH_NVDEC, NVCFB0_SEMAPHORE_D, 1);
        pb_data(ch, nv_completion_awaken(ch, VIDEO_COMPLETION_AWAKEN));
    #undef VAP
    #undef VAS

    kinfo("nv-chan", "NVDEC: decoding the %ux%u I_PCM IDR (%u-byte stream) on the card",
          (u32)H264_TEST_WIDTH, (u32)H264_TEST_HEIGHT, stream_len);
    bool consumed = submit_and_wait(ch, start, signal);
    if (!consumed) {
        kwarn("nv-chan", "NVDEC did not retire; retaining its buffers and rejecting incomplete decode output");
        return 1;
    }

    /* Read engine output coherently, not from the untouched sysmem staging
     * shadow.  Constant 0x80 remains invariant under the surface tiling. */
    u32 midgray = 0, neutral_chroma = 0;
    static u8 luma_sample[H264_TEST_WIDTH * H264_TEST_HEIGHT]
        __attribute__((aligned(4)));
    static u8 chroma_sample[H264_TEST_WIDTH * H264_TEST_HEIGHT / 2]
        __attribute__((aligned(4)));
    nvdec_frame_status_t status;
    if (!nv_vram_object_read(ch, H_NVDEC_VRAM, g_nvdec_vram_fb,
                             lum_off, luma_sample, sizeof luma_sample) ||
        !nv_vram_object_read(ch, H_NVDEC_VRAM, g_nvdec_vram_fb,
                             chr_off, chroma_sample, sizeof chroma_sample) ||
        !nv_vram_object_read(ch, H_NVDEC_VRAM, g_nvdec_vram_fb,
                             sts_off, &status, sizeof status)) {
        kerr("nv-chan", "NVDEC: coherent output/status readback failed");
        return 1;
    }
    for (u32 i = 0; i < sizeof luma_sample; i++)
        if (luma_sample[i] == 0x80) midgray++;
    for (u32 i = 0; i < sizeof chroma_sample; i++)
        if (chroma_sample[i] == 0x80) neutral_chroma++;
    u32 logical_pixels = H264_TEST_WIDTH * H264_TEST_HEIGHT;
    kinfo("nv-chan", "NVDEC: decoded MB=%u error MB=%u error=%#x slice-error=%#x cycles=%u; Y=%u/%u UV=%u/%u expected-neutral bytes",
          status.mbs_correctly_decoded, status.mbs_in_error, status.error_status,
          status.slice_header_error_code, status.cycle_count,
          midgray, logical_pixels, neutral_chroma, logical_pixels / 2u);
    if (consumed && status.mbs_correctly_decoded == logical_pixels / 256u &&
        !status.mbs_in_error && !status.error_status && !status.slice_header_error_code &&
        midgray == logical_pixels && neutral_chroma == logical_pixels / 2u) {
        kinfo("nv-chan", "NVDEC DECODED the I-frame on the real card: full NV12 Y/UV and clean status verified");
        return 0;
    }
    kwarn("nv-chan", "NVDEC decode unproven: decoded counts, status or complete NV12 pixels did not match");
    return 1;   /* 0 = pass, 1 = ran-but-wrong-pixels (setup/not-open returns -1 above) */
}

/* Exercise the same parsed-stream NVDEC path exposed to applications. Only
 * actual retired NVENC bytes enter this path; never substitute the boot fixture.
 * This boot test is serialized before userland and owns its private snapshots. */
int nv_nvenc_roundtrip_hw(void) {
    nv_channel_t *ch=&channels[CH_NVDEC];
    if (!ch->open) return -1;
    if (ch->submit_failed) return 1;
    enum { WIDTH=NVENC_H264_TEST_WIDTH, HEIGHT=NVENC_H264_TEST_HEIGHT,
           Y_BYTES=WIDTH*HEIGHT, UV_BYTES=Y_BYTES/2, PIXELS=Y_BYTES+UV_BYTES };
    static u8 encoded[NVENC_H264_STREAM_CAPACITY];
    static u8 decoded[PIXELS];
    kh264_decode_desc desc;
    u32 bytes=0;
    if (!nv_nvenc_test_stream(encoded,sizeof encoded,&bytes,&desc)) {
        kwarn("nv-chan", "NVENC-to-NVDEC: no valid retired encoder stream/context; round trip not run");
        return -1;
    }
    if (desc.coded_width!=WIDTH || desc.coded_height!=HEIGHT ||
        desc.display_width!=WIDTH || desc.display_height!=HEIGHT ||
        desc.crop_left || desc.crop_top || desc.crop_right || desc.crop_bottom) {
        kerr("nv-chan", "NVENC-to-NVDEC: unexpected encoder geometry");
        return 1;
    }
    /* Poison every byte: an unwritten or partial readback cannot match neutral
     * input. The decoder publishes success only after exact status, full planes,
     * untile and safe VRAM teardown. It never entropy-decodes on the CPU. */
    memset(decoded,0xa5,sizeof decoded);
    kvideo_request_t info={.version=KVIDEO_ABI,.operation=KVIDEO_H264_DECODE_IDR};
    kinfo("nv-chan", "NVENC-to-NVDEC: decoding actual %u-byte stream through application NVDEC path (%ux%u)",
          bytes,(u32)WIDTH,(u32)HEIGHT);
    int rc=nv_nvdec_decode_idr(encoded,bytes,decoded,sizeof decoded,&info);
    if (rc || info.phase!=KVIDEO_PHASE_COMPLETE || info.parse_status ||
        info.firmware_error || info.slice_error || info.error_mbs ||
        info.decoded_mbs!=Y_BYTES/256 || info.required_bytes!=PIXELS ||
        info.written_bytes!=PIXELS || info.coded_width!=WIDTH || info.coded_height!=HEIGHT ||
        info.display_width!=WIDTH || info.display_height!=HEIGHT || info.pitch!=WIDTH ||
        info.crop_left || info.crop_top || info.crop_right || info.crop_bottom) {
        kwarn("nv-chan", "NVENC-to-NVDEC: FAIL rc=%d phase=%u parse=%u firmware=%#x slice=%#x MB=%u error-MB=%u bytes=%u/%u",
              rc,info.phase,info.parse_status,info.firmware_error,info.slice_error,
              info.decoded_mbs,info.error_mbs,info.written_bytes,info.required_bytes);
        return 1;
    }
    u32 y_ok=0,uv_ok=0;
    for (u32 i=0;i<Y_BYTES;i++) if (decoded[i]==0x80) y_ok++;
    for (u32 i=0;i<UV_BYTES;i++) if (decoded[Y_BYTES+i]==0x80) uv_ok++;
    bool pass=y_ok==Y_BYTES && uv_ok==UV_BYTES;
    kinfo("nv-chan", "NVENC-to-NVDEC round trip %s: MB=%u Y=%u/%u UV=%u/%u (actual stream, dynamic buffers, full NV12)",
          pass?"PASS":"FAIL",info.decoded_mbs,y_ok,(u32)Y_BYTES,uv_ok,(u32)UV_BYTES);
    return pass?0:1;
}

/* Application-supplied, independently decodable H.264 frames. A dedicated
 * transient allocation keeps these jobs away from boot fixtures and graphics.
 * Every call is serialized by the shared render transaction. Failed fences or
 * uncertain RM/VMM teardown quarantine this slot; never reuse a live mapping. */
#define VA_NVDEC_USER       0x00a00000000ull
#define VA_NVDEC_USER_TILED 0x00a80000000ull
#define H_NVDEC_USER_VRAM   0x004e8000u
typedef struct {
    u64 fb;
    u32 bytes;
    bool allocated, linear_attempted, tiled_attempted, quarantined;
} nvdec_user_buffer_t;
static nvdec_user_buffer_t g_nvdec_user_buffer;

static bool nvdec_user_release(nv_channel_t *ch) {
    nvdec_user_buffer_t *b = &g_nvdec_user_buffer;
    if (b->quarantined || ch->submit_failed) { b->quarantined=true; return false; }
    if (b->tiled_attempted) {
        if (!nv_vmm_unmap(&ch->vmm, VA_NVDEC_USER_TILED, b->bytes) ||
            !nv_vmm_commit(ch->card,ch->rm,&ch->vmm,h_vaspace(CH_NVDEC),"video tiled unmap"))
            goto quarantine;
        b->tiled_attempted=false;
    }
    if (b->linear_attempted) {
        if (!nv_vmm_unmap(&ch->vmm, VA_NVDEC_USER, b->bytes) ||
            !nv_vmm_commit(ch->card,ch->rm,&ch->vmm,h_vaspace(CH_NVDEC),"video linear unmap"))
            goto quarantine;
        b->linear_attempted=false;
    }
    if (b->allocated && !nv_rm_free(ch->card,ch->rm,RM_DEVICE,H_NVDEC_USER_VRAM))
        goto quarantine;
    memset(b,0,sizeof *b);
    return true;
quarantine:
    b->quarantined=true;
    return false;
}

static bool nvdec_user_allocate(nv_channel_t *ch, u32 bytes) {
    nvdec_user_buffer_t *b=&g_nvdec_user_buffer;
    if (b->quarantined || b->allocated || b->linear_attempted || b->tiled_attempted ||
        !bytes || (bytes&0xffffu) || bytes >= VA_NVDEC_USER_TILED-VA_NVDEC_USER)
        return false;
    b->bytes=bytes;
    mem_alloc_params_t mp={0};
    mp.owner=0x4b455354u; mp.type=NVOS32_TYPE_IMAGE;
    mp.attr=NVOS32_ATTR_VIDMEM_CONTIGUOUS;
    mp.attr2=(NVOS32_ATTR2_GPU_CACHEABLE_NO<<2);
    mp.flags=NVOS32_ALLOC_FLAGS_NO_SCANOUT|NVOS32_ALLOC_FLAGS_ALIGN_FORCE;
    mp.size=bytes; mp.alignment=0x10000u;
    if (!nv_rm_alloc(ch->card,ch->rm,RM_DEVICE,H_NVDEC_USER_VRAM,
                     NV01_MEMORY_LOCAL_USER,&mp,sizeof mp)) {
        /* An opaque RM failure may be partial: do not retry this fixed handle. */
        b->quarantined=true; return false;
    }
    b->allocated=true;
    phys_attr_params_t pa={0}; u32 got=0;
    if (!nv_rm_control(ch->card,ch->rm,H_NVDEC_USER_VRAM,
            NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR,&pa,sizeof pa,&pa,sizeof pa,&got) ||
        got<sizeof pa || pa.mem_aperture!=NV0041_APERTURE_VIDMEM ||
        pa.contig_segment_size<bytes || (pa.mem_offset&0xffffu)) goto fail;
    b->fb=pa.mem_offset;
    b->linear_attempted=true;
    if (!nv_vmm_map(&ch->vmm,VA_NVDEC_USER,b->fb,bytes,true,false,false)) goto fail;
    b->tiled_attempted=true;
    if (!nv_vmm_map_kind(&ch->vmm,VA_NVDEC_USER_TILED,b->fb,bytes,
                         true,false,false,NV_VIDEO_PTE_KIND) ||
        !nv_vmm_commit(ch->card,ch->rm,&ch->vmm,h_vaspace(CH_NVDEC),"application NVDEC buffers"))
        goto fail;
    return true;
fail:
    (void)nvdec_user_release(ch);
    return false;
}

int nv_nvdec_decode_idr(const unsigned char *input, unsigned int bytes,
                       unsigned char *output, unsigned int capacity,
                       kvideo_request_t *info) {
    if (!info || info->version!=KVIDEO_ABI || info->operation>KVIDEO_H264_DECODE_IDR)
        return -E_INVAL;
    /* Do not expose stale success/error state from a preceding call. */
    info->coded_width=info->coded_height=info->display_width=info->display_height=0;
    info->crop_left=info->crop_top=info->crop_right=info->crop_bottom=0;
    info->pitch=info->required_bytes=info->written_bytes=0;
    info->parse_status=info->firmware_error=info->slice_error=0;
    info->decoded_mbs=info->error_mbs=info->reserved=0;
    info->phase=KVIDEO_PHASE_HEADERS;
    kh264_decode_desc desc;
    info->parse_status=kh264_parse_annexb(input,bytes,&desc);
    if (info->parse_status!=KH264_OK) return -E_INVAL;
    info->phase=KVIDEO_PHASE_LAYOUT;
    nvdec_h264_job_layout job;
    info->parse_status=nvdec_h264_job_plan(&job,&desc,bytes);
    if (info->parse_status!=KH264_OK) return -E_INVAL;
    nv_video_nv12_layout_s tiled,linear;
    if (!nv_video_nv12_layout_init(&tiled,NV_VIDEO_BLACKWELL_GOB2_NV12,
            desc.coded_width,desc.coded_height,job.pitch,job.pitch,
            job.luma_height,job.chroma_height) ||
        !nv_video_nv12_layout_init(&linear,NV_VIDEO_LINEAR_NV12,
            desc.coded_width,desc.coded_height,0,0,0,0) ||
        job.chroma_offset!=job.luma_offset+tiled.uv_offset ||
        job.luma_bytes!=tiled.y_bytes || job.chroma_bytes!=tiled.uv_bytes ||
        linear.bytes>KVIDEO_OUTPUT_MAX) return -E_INVAL;
    info->coded_width=desc.coded_width; info->coded_height=desc.coded_height;
    info->display_width=desc.display_width; info->display_height=desc.display_height;
    info->crop_left=desc.crop_left; info->crop_top=desc.crop_top;
    info->crop_right=desc.crop_right; info->crop_bottom=desc.crop_bottom;
    info->pitch=linear.y_pitch; info->required_bytes=(u32)linear.bytes;
    if (info->operation==KVIDEO_H264_INSPECT) {
        info->phase=KVIDEO_PHASE_COMPLETE; return 0;
    }
    if (!output || capacity<linear.bytes) return -E_INVAL;
    nv_channel_t *ch=&channels[CH_NVDEC];
    if (!ch->open) return -E_NOSYS;
    if (ch->submit_failed || g_nvdec_user_buffer.quarantined) return -E_IO;
    info->phase=KVIDEO_PHASE_ALLOCATE;
    u8 *stage=kmalloc(job.total_bytes);
    if (!stage) return -E_NOMEM;
    int result=-E_IO;
    memset(stage,0,job.total_bytes);
    nvdec_h264_pic_s pic;
    info->parse_status=nvdec_h264_parse_pic(&pic,&desc,input,bytes,job.pitch,job.pitch,
                                           job.history_bytes,job.mbhist_bytes);
    if (info->parse_status!=KH264_OK) { result=-E_INVAL; goto done; }
    memcpy(stage+job.stream_offset,input,bytes);
    memcpy(stage+job.stream_offset+bytes,pic.eos,sizeof pic.eos);
    memcpy(stage+job.picture_offset,&pic,sizeof pic);
    ((u32 *)(stage+job.slice_offset))[0]=desc.slice_start;
    ((u32 *)(stage+job.slice_offset))[1]=desc.slice_end;
    memset(stage+job.status_offset,0xff,job.status_bytes);
    if (!nvdec_user_allocate(ch,job.total_bytes)) goto done;
    info->phase=KVIDEO_PHASE_UPLOAD;
    if (!nv_vram_object_write(ch,H_NVDEC_USER_VRAM,g_nvdec_user_buffer.fb,
                               0,stage,job.total_bytes)) goto release;
    info->phase=KVIDEO_PHASE_SUBMIT;
    if (!pb_reserve(ch,192u)) goto release;
    u32 start=ch->pb_at;
    #define UDV(off) ((u32)((VA_NVDEC_USER+(off))>>8))
    #define UDS(off) ((u32)((VA_NVDEC_USER_TILED+(off))>>8))
    pb_method(ch,SUBCH_NVDEC,0,1); pb_data(ch,NVCFB0_VIDEO_DECODER|NV906F_SET_OBJECT_ENGINE_SW);
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_APPLICATION_ID,1); pb_data(ch,NVCFB0_SET_APPLICATION_ID_ID_H264);
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_CONTROL_PARAMS,1); pb_data(ch,NVCFB0_H264_CONTROL_PARAMS);
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_DRV_PIC_SETUP_OFFSET,1); pb_data(ch,UDV(job.picture_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_IN_BUF_BASE_OFFSET,1); pb_data(ch,UDV(job.stream_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_PICTURE_INDEX,1); pb_data(ch,0);
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_SLICE_OFFSETS_BUF_OFFSET,1); pb_data(ch,UDV(job.slice_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_COLOC_DATA_OFFSET,1); pb_data(ch,UDV(job.coloc_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_HISTORY_OFFSET,1); pb_data(ch,UDV(job.history_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_NVDEC_STATUS_OFFSET,1); pb_data(ch,UDV(job.status_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_PICTURE_LUMA_OFFSET0,1); pb_data(ch,UDS(job.luma_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SET_PICTURE_CHROMA_OFFSET0,1); pb_data(ch,UDS(job.chroma_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_H264_SET_MBHIST_BUF_OFFSET,1); pb_data(ch,UDV(job.mbhist_offset));
    pb_method(ch,SUBCH_NVDEC,NVCFB0_EXECUTE,1); pb_data(ch,NVCFB0_EXECUTE_VALUE);
    u32 signal=next_completion_signal(ch,0x56440000u);
    ch->sem[0]=0; cache_flush((const void *)&ch->sem[0],4);
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SEMAPHORE_A,3);
    pb_data(ch,(u32)(VA_SEM>>32)); pb_data(ch,(u32)VA_SEM); pb_data(ch,signal);
    pb_method(ch,SUBCH_NVDEC,NVCFB0_SEMAPHORE_D,1);
        pb_data(ch, nv_completion_awaken(ch, VIDEO_COMPLETION_AWAKEN));
    #undef UDV
    #undef UDS
    if (!submit_and_wait(ch,start,signal)) {
        g_nvdec_user_buffer.quarantined=true; goto done;
    }
    info->phase=KVIDEO_PHASE_STATUS;
    nvdec_frame_status_t status;
    if (!nv_vram_object_read(ch,H_NVDEC_USER_VRAM,g_nvdec_user_buffer.fb,
                              job.status_offset,&status,sizeof status)) goto release;
    info->firmware_error=status.error_status; info->slice_error=status.slice_header_error_code;
    info->decoded_mbs=status.mbs_correctly_decoded; info->error_mbs=status.mbs_in_error;
    if (status.error_status || status.slice_header_error_code || status.mbs_in_error ||
        status.mbs_correctly_decoded!=desc.width_mbs*desc.height_mbs) goto release;
    info->phase=KVIDEO_PHASE_READBACK;
    if (!nv_vram_object_read(ch,H_NVDEC_USER_VRAM,g_nvdec_user_buffer.fb,
                              job.luma_offset,stage+job.luma_offset,job.luma_bytes) ||
        !nv_vram_object_read(ch,H_NVDEC_USER_VRAM,g_nvdec_user_buffer.fb,
                              job.chroma_offset,stage+job.chroma_offset,job.chroma_bytes)) goto release;
    info->phase=KVIDEO_PHASE_UNTILE;
    if (!nv_video_nv12_convert(output,capacity,&linear,stage+job.luma_offset,tiled.bytes,&tiled))
        goto release;
    result=0;
release:
    if (!nvdec_user_release(ch)) { info->phase=KVIDEO_PHASE_RELEASE; result=-E_IO; }
    if (!result) { info->phase=KVIDEO_PHASE_COMPLETE; info->written_bytes=info->required_bytes; }
done:
    kfree(stage); /* GPU saw only VRAM, never this private CPU shadow. */
    return result;
}

#endif /* NV_CHAN_HOST_TEST */
