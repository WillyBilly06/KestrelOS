#!/usr/bin/env python3
"""Exercise the production BE PCIe DLFW pre-init against a register model."""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/rtw89.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint32_t u32;
#define R_BE_HAXI_INIT_CFG1 0xB000
#define R_BE_HAXI_DMA_STOP1 0xB010
#define R_BE_CMAC_FUNC_EN 0x10000
#define B_BE_CMAC_TXEN (1u<<29)
#define B_BE_CMAC_RXEN (1u<<28)
#define B_BE_TXDMA_EN (1u<<4)
#define B_BE_RXDMA_EN (1u<<5)
#define B_BE_STOP_WPDMA (1u<<31)
#define R_BE_TXBD_RWPTR_CLR1 0xB014
#define R_BE_RXBD_RWPTR_CLR1_V1 0xB018
#define B_BE_CLR_RXQ0_IDX 1u
#define B_BE_CLR_RPQ0_IDX 2u
#define BE_ALL_TX_CHANNELS 0x7fff
#define R_BE_DMAC_FUNC_EN 0x8400
#define R_BE_DMAC_CLK_EN 0x8404
#define B_BE_DLE_WDE_EN (1u<<26)
#define B_BE_DLE_PLE_EN (1u<<23)
#define B_BE_DLE_WDE_CLK_EN (1u<<26)
#define B_BE_DLE_PLE_CLK_EN (1u<<23)
#define R_BE_WDE_PKTBUF_CFG 0x8C08
#define R_BE_PLE_PKTBUF_CFG 0x9008
#define B_BE_WDE_PAGE_SEL_MASK 3u
#define B_BE_WDE_START_BOUND_MASK 0x7f00u
#define B_BE_WDE_FREE_PAGE_NUM_MASK 0x1fff0000u
#define B_BE_PLE_PAGE_SEL_MASK 3u
#define B_BE_PLE_START_BOUND_MASK 0x7f00u
#define B_BE_PLE_FREE_PAGE_NUM_MASK 0x1fff0000u
#define PLE_PAGE_SEL_128 1u
#define RTW89_PLE_START_OFFSET 212992u
#define DLE_BOUND_UNIT 8192u
#define R_BE_CH_PAGE_CTRL 0xB704
#define R_BE_HCI_FC_CTRL 0xB700
static u32 regs[0x11000/4], polls;
static bool finish=true, idle=true;
static u32 absent_status;
static u32 rd32(volatile u8 *r,u32 off){(void)r;return absent_status==off?0xffffffffu:regs[off/4];}
static void wr32(volatile u8 *r,u32 off,u32 v){
    (void)r;regs[off/4]=v;
    if(off==R_BE_DMAC_FUNC_EN && (v&(B_BE_DLE_WDE_EN|B_BE_DLE_PLE_EN)) && finish)
        regs[0x8d00/4]=regs[0x9100/4]=3;
}
static void set32(volatile u8 *r,u32 o,u32 b){wr32(r,o,rd32(r,o)|b);}
static void clr32(volatile u8 *r,u32 o,u32 b){wr32(r,o,rd32(r,o)&~b);}
static void timer_udelay(int us){assert(us==1);polls++;}
static void kerr(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
static bool wait_idle(volatile u8 *r,const char *who){assert(r&&who);return idle;}
'''
code += function(src, 'rtw89_fwdl_preinit') + r'''
'''
code += function(src, 'rtw89_dma_quiesce') + r'''
int main(void){
    volatile u8 *mmio=(volatile u8*)regs;
    regs[R_BE_HAXI_DMA_STOP1/4]=0xffffffffu;
    regs[R_BE_HAXI_INIT_CFG1/4]=7u<<8;
    assert(rtw89_fwdl_preinit(mmio,"8922"));
    assert((regs[0x7880/4]&3u)==3u);
    assert(!(regs[R_BE_HAXI_INIT_CFG1/4]&(7u<<8)));
    assert(!(regs[R_BE_HAXI_DMA_STOP1/4]&BE_ALL_TX_CHANNELS));
    assert(regs[0x8420/4]&(1u<<12));
    assert(regs[R_BE_WDE_PKTBUF_CFG/4]==0);
    assert(regs[R_BE_PLE_PKTBUF_CFG/4]==(1u|(26u<<8)|(2928u<<16)));
    for(unsigned i=0;i<5;i++)assert(regs[(0x8c40+i*4)/4]==(i==1?0x60006u:0));
    for(unsigned i=0;i<13;i++){
        u32 q=i==2?32:i==3?256:i==10?1:0;
        assert(regs[(0x9040+i*4)/4]==((q<<16)|q));
    }
    assert(regs[R_BE_HCI_FC_CTRL/4]==8);
    assert(regs[R_BE_CH_PAGE_CTRL/4]==0);
    regs[0x8d00/4]=regs[0x9100/4]=0;finish=false;
    assert(!rtw89_fwdl_preinit(mmio,"8922"));
    assert(polls>=2000);
    assert(!(regs[R_BE_DMAC_FUNC_EN/4]&(B_BE_DLE_WDE_EN|B_BE_DLE_PLE_EN)));
    for(unsigned engine=0;engine<2;++engine){
        finish=true;absent_status=engine?0x9100:0x8d00;polls=0;
        assert(!rtw89_fwdl_preinit(mmio,"8922")&&!polls);
        assert(!(regs[R_BE_DMAC_FUNC_EN/4]&(B_BE_DLE_WDE_EN|B_BE_DLE_PLE_EN)));
    }
    absent_status=0;
    regs[R_BE_CMAC_FUNC_EN/4]=B_BE_CMAC_TXEN|B_BE_CMAC_RXEN;
    regs[R_BE_HAXI_INIT_CFG1/4]=B_BE_TXDMA_EN|B_BE_RXDMA_EN;
    assert(rtw89_dma_quiesce(mmio,"8922"));
    assert(!(regs[R_BE_CMAC_FUNC_EN/4]&(B_BE_CMAC_TXEN|B_BE_CMAC_RXEN)));
    assert(!(regs[R_BE_HAXI_INIT_CFG1/4]&(B_BE_TXDMA_EN|B_BE_RXDMA_EN)));
    assert((regs[R_BE_HAXI_DMA_STOP1/4]&(B_BE_STOP_WPDMA|BE_ALL_TX_CHANNELS))
           ==(B_BE_STOP_WPDMA|BE_ALL_TX_CHANNELS));
    assert(regs[R_BE_TXBD_RWPTR_CLR1/4]==BE_ALL_TX_CHANNELS);
    assert(regs[R_BE_RXBD_RWPTR_CLR1_V1/4]==3);
    idle=false;regs[R_BE_TXBD_RWPTR_CLR1/4]=0;
    assert(!rtw89_dma_quiesce(mmio,"8922"));
    assert(regs[R_BE_TXBD_RWPTR_CLR1/4]==0);
    puts("PASS RTL8922AE production DLFW pre-init and BE PCIe quiesce/refusal");
}
'''
run_test(code, 'rtw89-fwdl-preinit')
