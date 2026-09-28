/* nv_qmd.h - the Blackwell compute launch descriptor (QMD).
 *
 * A QMD (Queue Meta Data) is the block the GPU's compute engine reads to launch
 * a kernel: where the shader machine code is, how many blocks and threads, how
 * many registers each thread uses, how much shared memory.  It is the consumer
 * for the sm_120 machine code tools/nvshader.py already produces and verifies -
 * the two halves of "run a shader on the card", of which this is the launch
 * half.
 *
 * The class is BLACKWELL_COMPUTE_B (0xCEC0, verified in nv_blackwell.c against
 * NVIDIA's clcec0.h) and its QMD is version 05_00, a bit-packed structure whose
 * field positions come from NVIDIA's own classes/compute/clcec0qmd.h.  Those
 * positions are the silent-failure kind (a field one bit off launches garbage),
 * so they are checked mechanically by tools/verify_nv_qmd.py, not by eye
 * ([[kestrelos-verify-transcribed-constants]]).
 *
 * This fills the fields a plain grid launch needs; the actual submission of the
 * finished QMD to a channel, and its execution, are the parts that need the GSP
 * up on real silicon (see kestrelos-gpu-two-walls).
 */
#ifndef KESTREL_NV_QMD_H
#define KESTREL_NV_QMD_H

#ifndef NV_QMD_HOST_TEST
#include "kernel.h"
#endif

/* The v05_00 QMD spans up to bit 3071; 96 words (384 bytes) holds it. */
#define NV_QMD_WORDS   96

/* A field is a contiguous bit range [lo, hi] within the QMD, exactly as
 * NVIDIA's header writes MW(hi:lo).  Named so verify_nv_qmd.py can pair each
 * with its NVCEC0_QMDV05_00_* definition. */
typedef struct { u16 lo, hi; } nv_qmd_field_t;

/* The launch-critical fields, from classes/compute/clcec0qmd.h (QMDV05_00). */
#define NV_QMD_F_QMD_TYPE                    ((nv_qmd_field_t){151, 153})
#define NV_QMD_F_QMD_GROUP_ID                ((nv_qmd_field_t){144, 149})
#define NV_QMD_F_QMD_MAJOR_VERSION           ((nv_qmd_field_t){468, 471})
#define NV_QMD_F_QMD_MINOR_VERSION           ((nv_qmd_field_t){464, 467})
#define NV_QMD_F_API_VISIBLE_CALL_LIMIT      ((nv_qmd_field_t){456, 456})
#define NV_QMD_F_SAMPLER_INDEX               ((nv_qmd_field_t){457, 457})
#define NV_QMD_F_SASS_VERSION                ((nv_qmd_field_t){448, 455})
#define NV_QMD_F_PROGRAM_PREFETCH_SIZE       ((nv_qmd_field_t){1077, 1085})
#define NV_QMD_F_BARRIER_COUNT               ((nv_qmd_field_t){1137, 1141})
/* Shared-memory HW carveout config (indices into the SM's shared-mem size
 * table); required alongside SHARED_MEMORY_SIZE on Blackwell (v05_00, the "_gb"
 * bounded variant).  gv100_smem_size_to_hw(kB) = kB/4 + 1; for a 0-shared-mem
 * kernel min=target=max=1 (smallest carveout). */
#define NV_QMD_F_MIN_SM_CONFIG_SHARED_MEM    ((nv_qmd_field_t){1163, 1168})
#define NV_QMD_F_MAX_SM_CONFIG_SHARED_MEM    ((nv_qmd_field_t){1169, 1174})
#define NV_QMD_F_TARGET_SM_CONFIG_SHARED_MEM ((nv_qmd_field_t){1175, 1180})
#define NV_QMD_F_PROGRAM_ADDRESS_LOWER       ((nv_qmd_field_t){1024, 1055})
#define NV_QMD_F_PROGRAM_ADDRESS_UPPER       ((nv_qmd_field_t){1056, 1076})
#define NV_QMD_F_CTA_THREAD_DIMENSION0       ((nv_qmd_field_t){1088, 1103})
#define NV_QMD_F_CTA_THREAD_DIMENSION1       ((nv_qmd_field_t){1104, 1119})
#define NV_QMD_F_CTA_THREAD_DIMENSION2       ((nv_qmd_field_t){1120, 1127})
#define NV_QMD_F_REGISTER_COUNT              ((nv_qmd_field_t){1128, 1136})
#define NV_QMD_F_SHARED_MEMORY_SIZE_SHIFTED7 ((nv_qmd_field_t){1152, 1162})
#define NV_QMD_F_SHADER_LOCAL_MEMORY_LOW_SIZE_SHIFTED4  ((nv_qmd_field_t){1184, 1199})
#define NV_QMD_F_SHADER_LOCAL_MEMORY_HIGH_SIZE_SHIFTED4 ((nv_qmd_field_t){1200, 1215})
#define NV_QMD_F_GRID_WIDTH                  ((nv_qmd_field_t){1248, 1279})
#define NV_QMD_F_GRID_HEIGHT                 ((nv_qmd_field_t){1280, 1295})
#define NV_QMD_F_GRID_DEPTH                  ((nv_qmd_field_t){1312, 1327})

/* Constant buffers, indexed by bank (0..7).  A CUDA kernel's arguments arrive
 * in bank 0, which the shader loads with LDC c[0x0][off]; the driver puts the
 * argument bytes in memory and points bank 0 here.  Address is stored >>6
 * (64-byte aligned), size >>4 (16-byte units). */
#define NV_QMD_F_CB_ADDR_LOWER(i) ((nv_qmd_field_t){(u16)(1344+(i)*64), (u16)(1375+(i)*64)})
#define NV_QMD_F_CB_ADDR_UPPER(i) ((nv_qmd_field_t){(u16)(1376+(i)*64), (u16)(1394+(i)*64)})
#define NV_QMD_F_CB_SIZE(i)       ((nv_qmd_field_t){(u16)(1395+(i)*64), (u16)(1407+(i)*64)})
#define NV_QMD_F_CB_VALID(i)      ((nv_qmd_field_t){(u16)(1856+(i)*4),  (u16)(1856+(i)*4)})

/* Cache-coherency + launch fields a WORKING Blackwell driver (tinygrad ops_nv.py)
 * sets, verified vs clcec0qmd.h: invalidate the SM texture/shader caches so a
 * shader/QMD freshly written into VRAM is not shadowed by stale cache lines,
 * SYSMEMBAR the CWD, and invalidate constant bank 0. */
#define NV_QMD_F_INV_TEX_HEADER_CACHE   ((nv_qmd_field_t){472, 472})
#define NV_QMD_F_INV_TEX_SAMPLER_CACHE  ((nv_qmd_field_t){473, 473})
#define NV_QMD_F_INV_TEX_DATA_CACHE     ((nv_qmd_field_t){474, 474})
#define NV_QMD_F_INV_SHADER_DATA_CACHE  ((nv_qmd_field_t){475, 475})
#define NV_QMD_F_CWD_MEMBAR_TYPE        ((nv_qmd_field_t){624, 625})
#define NV_QMD_F_CB_INVALIDATE0         ((nv_qmd_field_t){1859, 1859})
#define NV_QMD_F_PROGRAM_PREFETCH_ADDR_LOWER ((nv_qmd_field_t){1888, 1919})
#define NV_QMD_F_PROGRAM_PREFETCH_ADDR_UPPER ((nv_qmd_field_t){1920, 1936})

#define NV_QMD_TYPE_GRID_CTA   0x2
#define NV_QMD_MAJOR_VERSION_5 0x5
#define NV_QMD_CWD_MEMBAR_L1_SYSMEMBAR 0x1

/* What a compute launch is: a shader at `program_addr`, a grid of
 * grid[x,y,z] blocks each of block[x,y,z] threads, using `reg_count` registers
 * and `shared_bytes` of shared memory.  CUDA's Blackwell sm_120 device-info
 * value 0xa04 encodes to QMD `sass_version` 0xa4; NAK-produced shaders may use
 * zero and leave the CUDA-specific launch fields clear. */
typedef struct {
    u64 program_addr;
    u32 grid[3];
    u32 block[3];
    u32 reg_count;
    u32 shared_bytes;
    u32 program_size;
    u32 shader_local_memory_low_bytes;
    u32 shader_local_memory_high_bytes;
    u8  sass_version;
    u8  sampler_index;
    u8  barrier_count;
    u8  qmd_group_id;
} nv_compute_launch_t;

/* Set field `f` of the QMD word array to `value`. */
void nv_qmd_set(u32 *qmd, nv_qmd_field_t f, u32 value);
/* Read it back (for tests). */
u32  nv_qmd_get(const u32 *qmd, nv_qmd_field_t f);

/* Zero `qmd` (NV_QMD_WORDS words) and fill it for a grid compute launch. */
void nv_qmd_build_compute(u32 *qmd, const nv_compute_launch_t *l);

/* Point constant-buffer bank `bank` at `addr` (in GPU memory) for `size` bytes
 * and mark it valid - how a kernel's arguments (bank 0) reach it. */
void nv_qmd_set_constant_buffer(u32 *qmd, u32 bank, u64 addr, u32 size);

#endif
