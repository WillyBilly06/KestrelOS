#!/usr/bin/env python3
"""Exercise production BE stop after a firmware download fails early."""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/rtw.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef struct {u8 *data;} firmware_t;
typedef struct wifi_device wifi_device_t;
typedef struct {
    volatile u8 *regs;
    bool is_be, firmware_running, fwdl_ring_attached, fwdl_ring_poisoned;
    bool mailbox_ready, data_ready, rx_ready, calibrated, header_read;
    u8 *fwdl_ring_memory, *fwdl_packet_memory;
    firmware_t fw;
} rtw_t;
struct wifi_device {
    void *ctx;
    bool unsupported_generation, radio_up;
    const char *model;
};
static int quiesces, warnings;
static bool can_quiesce=true;
static bool rtw89_dma_quiesce(volatile u8 *regs,const char *who){
    assert(regs&&who);quiesces++;return can_quiesce;
}
static void kwarn(const char *tag,const char *fmt,...){
    (void)tag;(void)fmt;warnings++;
}
static void hold_cpu(rtw_t *c){(void)c;assert(0);}
static void firmware_free(firmware_t *f){(void)f;assert(0);}
'''
code += function(src, 'rtw_stop') + r'''
int main(void){
    u8 mmio=0,ring=0,packet=0;
    rtw_t c={.regs=&mmio,.is_be=true,.fwdl_ring_attached=true,
             .firmware_running=false,.mailbox_ready=true,.data_ready=true,
             .rx_ready=true,.calibrated=true,.fwdl_ring_memory=&ring,
             .fwdl_packet_memory=&packet};
    wifi_device_t d={.ctx=&c,.model="8922",.radio_up=true};
    rtw_stop(&d); /* attached CH12 but FW never became ready */
    assert(quiesces==1&&!warnings);
    assert(c.fwdl_ring_poisoned&&!c.firmware_running&&!c.mailbox_ready);
    assert(!c.data_ready&&!c.rx_ready&&!c.calibrated&&!d.radio_up);
    assert(c.fwdl_ring_memory==&ring&&c.fwdl_packet_memory==&packet);
    c.fwdl_ring_poisoned=false;c.mailbox_ready=true;d.radio_up=true;
    can_quiesce=false;
    rtw_stop(&d);
    assert(quiesces==2&&warnings==1&&c.fwdl_ring_poisoned);
    assert(c.fwdl_ring_memory==&ring&&c.fwdl_packet_memory==&packet);
    c.fwdl_ring_attached=false;c.fwdl_ring_poisoned=false;d.radio_up=true;
    rtw_stop(&d);
    assert(quiesces==2&&c.fwdl_ring_poisoned&&!d.radio_up);
    puts("PASS RTL8922AE BE stop: quiesce attached ring before FW ready, retain DMA backing and poison on failure");
}
'''
run_test(code, 'rtw89-fwdl-stop')
