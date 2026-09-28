/* nv_qmd.c - build a Blackwell compute launch descriptor.  See nv_qmd.h.
 * Field positions verified by tools/verify_nv_qmd.py against NVIDIA's
 * classes/compute/clcec0qmd.h. */
#ifdef NV_QMD_HOST_TEST
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
#include <string.h>
#include "nv_qmd.h"
#else
#include "nv_qmd.h"
#endif

void nv_qmd_set(u32 *qmd, nv_qmd_field_t f, u32 value) {
    for (u32 b = f.lo; b <= f.hi; b++) {
        u32 bit = (value >> (b - f.lo)) & 1u;
        if (bit) qmd[b >> 5] |=  (1u << (b & 31));
        else     qmd[b >> 5] &= ~(1u << (b & 31));
    }
}

u32 nv_qmd_get(const u32 *qmd, nv_qmd_field_t f) {
    u32 v = 0;
    for (u32 b = f.lo; b <= f.hi; b++) {
        u32 bit = (qmd[b >> 5] >> (b & 31)) & 1u;
        v |= bit << (b - f.lo);
    }
    return v;
}

void nv_qmd_build_compute(u32 *qmd, const nv_compute_launch_t *l) {
    for (int i = 0; i < NV_QMD_WORDS; i++) qmd[i] = 0;

    nv_qmd_set(qmd, NV_QMD_F_QMD_TYPE, NV_QMD_TYPE_GRID_CTA);
    nv_qmd_set(qmd, NV_QMD_F_QMD_MAJOR_VERSION, NV_QMD_MAJOR_VERSION_5);
    /* v05_00 constants Mesa's Qmd5_0::new sets unconditionally (qmd.rs): minor
     * version 0, group id 0x1f, API_VISIBLE_CALL_LIMIT=NO_CHECK(1),
     * SAMPLER_INDEX=INDEPENDENTLY(0).  Omitting the group id / call limit leaves
     * the scheduler reading 0 (a different, wrong config). */
    nv_qmd_set(qmd, NV_QMD_F_QMD_MINOR_VERSION, 0);
    nv_qmd_set(qmd, NV_QMD_F_QMD_GROUP_ID,
               l->qmd_group_id ? l->qmd_group_id : 0x1f);
    nv_qmd_set(qmd, NV_QMD_F_API_VISIBLE_CALL_LIMIT, 1);   /* NO_CHECK */
    nv_qmd_set(qmd, NV_QMD_F_SAMPLER_INDEX, l->sampler_index);
    nv_qmd_set(qmd, NV_QMD_F_SASS_VERSION, l->sass_version);

    /* The program address is stored shifted right by four - the shader is at
     * least 16-byte aligned, so its low four bits are always zero and the
     * field holds the rest. */
    u64 prog = l->program_addr >> 4;
    nv_qmd_set(qmd, NV_QMD_F_PROGRAM_ADDRESS_LOWER, (u32)(prog & 0xFFFFFFFFu));
    nv_qmd_set(qmd, NV_QMD_F_PROGRAM_ADDRESS_UPPER, (u32)(prog >> 32));

    /* Blackwell's CUDA/ptxas path supplies an explicit program-prefetch range.
     * NAK can leave this zero, so make it data-driven rather than imposing it
     * on every shader producer.  The address is stored >>8 and the size is in
     * 256-byte units (clcec0qmd.h; tinygrad's working ptxas path). */
    if (l->program_size) {
        u32 prefetch = l->program_size >> 8;
        if (prefetch > 0x1ffu) prefetch = 0x1ffu;
        u64 prefetch_addr = l->program_addr >> 8;
        nv_qmd_set(qmd, NV_QMD_F_PROGRAM_PREFETCH_SIZE, prefetch);
        nv_qmd_set(qmd, NV_QMD_F_PROGRAM_PREFETCH_ADDR_LOWER,
                   (u32)prefetch_addr);
        nv_qmd_set(qmd, NV_QMD_F_PROGRAM_PREFETCH_ADDR_UPPER,
                   (u32)(prefetch_addr >> 32));
    }

    nv_qmd_set(qmd, NV_QMD_F_CTA_THREAD_DIMENSION0, l->block[0]);
    nv_qmd_set(qmd, NV_QMD_F_CTA_THREAD_DIMENSION1, l->block[1]);
    nv_qmd_set(qmd, NV_QMD_F_CTA_THREAD_DIMENSION2, l->block[2]);

    nv_qmd_set(qmd, NV_QMD_F_REGISTER_COUNT, l->reg_count);
    nv_qmd_set(qmd, NV_QMD_F_BARRIER_COUNT, l->barrier_count);

    nv_qmd_set(qmd, NV_QMD_F_SHADER_LOCAL_MEMORY_LOW_SIZE_SHIFTED4,
               (l->shader_local_memory_low_bytes + 15u) >> 4);
    nv_qmd_set(qmd, NV_QMD_F_SHADER_LOCAL_MEMORY_HIGH_SIZE_SHIFTED4,
               (l->shader_local_memory_high_bytes + 15u) >> 4);

    /* Shared memory is stored in units of 128 bytes (shifted right by 7). */
    nv_qmd_set(qmd, NV_QMD_F_SHARED_MEMORY_SIZE_SHIFTED7,
               (l->shared_bytes + 127u) >> 7);
    /* Blackwell (v05_00) also needs the HW shared-mem carveout config indices
     * (gv100_smem_size_to_hw = kB/4 + 1).  For the common 0-shared-mem kernel
     * that is index 1 for all three (smallest carveout).  A kernel that uses
     * shared memory must raise these per the SM's shared-mem size table. */
    {
        u32 kb = (l->shared_bytes + 1023u) >> 10;         /* round up to kB */
        u32 min_hw = (kb / 4u) + 1u;                      /* smallest that fits  */
        /* MAX is ALWAYS the SM's LARGEST carveout, independent of this kernel's
         * shared_bytes (NVK gv100_get_hw_smem_sizes: max = last table entry).
         * Blackwell's table is {0,8,16,32,64,100} KB (Mesa nouveau_device.c
         * init_shared_mem_sizes, sm>=120), so MAX = 100/4+1 = 26.  The old code
         * set MAX to the SMALLEST (=1), giving the SM an invalid config ceiling
         * that faulted GR with GR_EXCEPTION on launch. */
        u32 max_hw = (100u / 4u) + 1u;                    /* 100 KB -> 26 (0x1a) */
        nv_qmd_set(qmd, NV_QMD_F_MIN_SM_CONFIG_SHARED_MEM,    min_hw);
        nv_qmd_set(qmd, NV_QMD_F_TARGET_SM_CONFIG_SHARED_MEM, min_hw);
        nv_qmd_set(qmd, NV_QMD_F_MAX_SM_CONFIG_SHARED_MEM,    max_hw);
    }

    nv_qmd_set(qmd, NV_QMD_F_GRID_WIDTH,  l->grid[0]);
    nv_qmd_set(qmd, NV_QMD_F_GRID_HEIGHT, l->grid[1]);
    nv_qmd_set(qmd, NV_QMD_F_GRID_DEPTH,  l->grid[2]);

    /* Cache-coherency + launch fields a working Blackwell driver sets (tinygrad
     * ops_nv.py): invalidate the SM texture/shader caches so the VRAM-resident
     * shader/QMD/cbuf are read fresh (not shadowed by stale lines after our PRAMIN
     * write), SYSMEMBAR the compute work-distributor, and invalidate constant
     * bank 0.  Positions verified vs clcec0qmd.h. */
    nv_qmd_set(qmd, NV_QMD_F_INV_TEX_HEADER_CACHE,  1);
    nv_qmd_set(qmd, NV_QMD_F_INV_TEX_SAMPLER_CACHE, 1);
    nv_qmd_set(qmd, NV_QMD_F_INV_TEX_DATA_CACHE,    1);
    nv_qmd_set(qmd, NV_QMD_F_INV_SHADER_DATA_CACHE, 1);
    nv_qmd_set(qmd, NV_QMD_F_CWD_MEMBAR_TYPE, NV_QMD_CWD_MEMBAR_L1_SYSMEMBAR);
    nv_qmd_set(qmd, NV_QMD_F_CB_INVALIDATE0, 1);
}

void nv_qmd_set_constant_buffer(u32 *qmd, u32 bank, u64 addr, u32 size) {
    u64 a = addr >> 6;                            /* 64-byte aligned */
    nv_qmd_set(qmd, NV_QMD_F_CB_ADDR_LOWER(bank), (u32)(a & 0xFFFFFFFFu));
    nv_qmd_set(qmd, NV_QMD_F_CB_ADDR_UPPER(bank), (u32)(a >> 32));
    nv_qmd_set(qmd, NV_QMD_F_CB_SIZE(bank), (size + 15u) >> 4);   /* 16-byte units */
    nv_qmd_set(qmd, NV_QMD_F_CB_VALID(bank), 1);
}
