/* Simulate what the GPU does with a compute dispatch: parse the launch
 * pushbuffer, follow the QMD pointer, pull the launch parameters out of the
 * QMD, and check they are the ones the driver intended.  Ties nv_qmd.c and
 * nv_compute.c together end to end - the "a model executes the stream" check,
 * done for compute the way nv_accel does it for 2D. */
#define NV_QMD_HOST_TEST
#define NV_COMPUTE_HOST_TEST
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
#include <stdio.h>
#include <string.h>
#include "nv_qmd.h"
#include "nv_compute.h"

static int fails=0;
#define CHECK(c,m) do{ if(!(c)){printf("  FAIL: %s\n",m);fails++;} }while(0)

/* A tiny slab of "GPU memory": the QMD lives here at a known address. */
static u8 gpumem[4096];
#define QMD_ADDR  0x600u          /* 256-byte aligned inside the slab */

int main(void){
    /* 1. Driver side: build the QMD for the addvec launch, place it in memory. */
    u32 qmd[NV_QMD_WORDS];
    nv_compute_launch_t l = { .program_addr=0x7f001230ULL, .grid={4096,1,1},
        .block={256,1,1}, .reg_count=12, .shared_bytes=0,
        .program_size=0x200, .sass_version=0xa4, .sampler_index=1,
        .barrier_count=1, .qmd_group_id=0x3f };
    nv_qmd_build_compute(qmd,&l);
    nv_qmd_set_constant_buffer(qmd, 0, 0x10000000ULL, 924);
    memcpy(gpumem + QMD_ADDR, qmd, sizeof qmd);

    /* 2. Driver side: build the launch pushbuffer that points at that QMD. */
    u32 pb[32]; u32 at=0;
    nv_compute_push_launch(pb,&at,32, 1 /*subc*/, 0xCEC0, QMD_ADDR);

    /* 3. GPU model: walk the pushbuffer, follow the QMD, extract the launch. */
    u32 bound_class=0; u64 got_qmd=0; int launched=0;
    for (u32 i=0; i<at; ) {
        u32 h=pb[i++]; u32 op=h>>29, cnt=(h>>16)&0x1FFF, mthd=(h&0x1FFF)<<2;
        CHECK(op==1,"pushbuffer op = increasing");
        for (u32 k=0;k<cnt;k++){
            u32 d=pb[i++];
            if (mthd==0x0000) bound_class=d;                 /* SET_OBJECT */
            else if (mthd==0x02b4) got_qmd=(u64)d<<8;         /* SEND_PCAS_A */
            else if (mthd==0x02c0 && (d&0xf)==3) launched=1;  /* PCAS2 invalidate/copy/schedule */
            if (!op) mthd+=4;                                 /* increasing */
        }
    }
    CHECK(bound_class==0xCEC0,"model saw BLACKWELL_COMPUTE_B bound");
    CHECK(got_qmd==QMD_ADDR,"model resolved the QMD address");
    CHECK(launched,"model saw the schedule signal");

    /* 4. GPU model: read the QMD it was pointed at, extract the launch params. */
    u32 *mq = (u32 *)(gpumem + got_qmd);
    u64 prog = ((u64)nv_qmd_get(mq,NV_QMD_F_PROGRAM_ADDRESS_UPPER)<<32 | nv_qmd_get(mq,NV_QMD_F_PROGRAM_ADDRESS_LOWER))<<4;
    u64 cb0  = ((u64)nv_qmd_get(mq,NV_QMD_F_CB_ADDR_UPPER(0))<<32 | nv_qmd_get(mq,NV_QMD_F_CB_ADDR_LOWER(0)))<<6;
    CHECK(nv_qmd_get(mq,NV_QMD_F_QMD_TYPE)==NV_QMD_TYPE_GRID_CTA,"QMD is a CTA grid");
    CHECK(prog==l.program_addr,"shader address matches what the driver set");
    CHECK(nv_qmd_get(mq,NV_QMD_F_GRID_WIDTH)==4096 && nv_qmd_get(mq,NV_QMD_F_CTA_THREAD_DIMENSION0)==256,"grid/block match");
    CHECK(nv_qmd_get(mq,NV_QMD_F_REGISTER_COUNT)==12,"register count matches");
    CHECK(nv_qmd_get(mq,NV_QMD_F_SASS_VERSION)==0xa4,"ptxas SASS version matches Blackwell");
    CHECK(nv_qmd_get(mq,NV_QMD_F_BARRIER_COUNT)==1,"ptxas ABI barrier is present");
    CHECK(nv_qmd_get(mq,NV_QMD_F_PROGRAM_PREFETCH_SIZE)==2,"program prefetch size matches");
    CHECK(cb0==0x10000000ULL && nv_qmd_get(mq,NV_QMD_F_CB_VALID(0))==1,"argument constant buffer matches");
    printf("  model launched class %04x: shader@%llx grid=%u block=%u regs=%u args@%llx\n",
        bound_class,(unsigned long long)prog,nv_qmd_get(mq,NV_QMD_F_GRID_WIDTH),
        nv_qmd_get(mq,NV_QMD_F_CTA_THREAD_DIMENSION0),nv_qmd_get(mq,NV_QMD_F_REGISTER_COUNT),
        (unsigned long long)cb0);
    printf(fails?"\nFAILED (%d)\n":"\nALL PASS\n",fails);
    return fails?1:0;
}
