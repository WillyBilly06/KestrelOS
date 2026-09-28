#!/usr/bin/env python3
"""Exercise the production RTL8922AE PCI firmware-download sender."""
from test_gpu_stable_candidate import ROOT, function, run_test

src = (ROOT / 'kernel/rtw.c').read_text()
ring_src = (ROOT / 'kernel/rtw89.c').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define BE_RXD_RPKT_LEN_MASK 0x3fffu
#define RTW89_PCI_BD_BYTES 8u
#define RTW89_PCI_TXBD_OPTION_LS (1u<<14)
#define R_BE_CH12_TXBD_IDX 0x1300u
#define R_BE_HALT_H2C_CTRL 0x160u
#define R_BE_HALT_C2H_CTRL 0x164u
typedef struct {u8 *bd;u64 bd_phys;u16 slots,host_index,card_index;} rtw89_ring_t;
typedef struct {
    volatile u8 *regs;
    const char *who;
    rtw89_ring_t ring;
    u8 *packet;
    u64 packet_phys;
    int sent;
    bool failed;
    bool expect_fwdl_path;
} be_push_t;
static u32 doors,waits,delay_calls;
static u16 hardware_done;
static bool path_ready=true, complete=true, quiet_refusals=true;
static void kerr(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
static void rtw89_ring_doorbell(volatile u8 *regs,rtw89_ring_t *r,u32 off){
    assert(regs&&r&&off==R_BE_CH12_TXBD_IDX);doors++;
    if(complete) hardware_done=r->host_index;
}
static u16 rtw89_ring_card_index(volatile u8 *regs,u32 off){
    assert(regs&&off==R_BE_CH12_TXBD_IDX);return hardware_done;
}
static bool rtw89_fwdl_path_ready(volatile u8 *regs,bool h2c,const char *who){
    assert(regs&&!h2c&&who&&strcmp(who,"8922")==0);waits++;return path_ready;
}
static void timer_mdelay(u32 ms){assert(ms==1);delay_calls++;}
'''
code += function(ring_src, 'rtw89_bd_write')
start = ring_src.index('u16 rtw89_ring_free(')
code += ring_src[start:ring_src.index('\n}', start) + 2]
code += function(ring_src, 'rtw89_ring_add')
code += function(src, 'be_send') + r'''
int main(void){
    u8 regs[0x200]={0},packet[4096]={0},bd[128*RTW89_PCI_BD_BYTES]={0};
    be_push_t p={.regs=regs,.who="8922",.packet=packet,.packet_phys=0x1000,
                 .expect_fwdl_path=true};
    p.ring.bd=bd;p.ring.slots=128;
    *(u32*)(regs+R_BE_HALT_H2C_CTRL)=0xffffffffu;
    *(u32*)(regs+R_BE_HALT_C2H_CTRL)=0xffffffffu;
    const u8 header[12]={1,2,3,4,5,6,7,8,9,10,11,12};
    assert(be_send(&p,header,sizeof header,false));
    assert(p.ring.host_index==1&&p.ring.card_index==1&&doors==1&&waits==1&&p.sent==1);
    assert(bd[0]==24+sizeof header&&bd[4]==0x00&&bd[5]==0x10);
    assert(((u32)packet[0] | ((u32)packet[1]<<8) | ((u32)packet[2]<<16) |
            ((u32)packet[3]<<24)) == (sizeof header | (13u<<24)));
    for(int i=4;i<24;i++)assert(!packet[i]);
    assert(memcmp(packet+24,header,sizeof header)==0);
    assert(*(u32*)(regs+R_BE_HALT_H2C_CTRL)==0);
    assert(*(u32*)(regs+R_BE_HALT_C2H_CTRL)==0);
    const u8 section[4]={0xaa,0xbb,0xcc,0xdd};
    assert(be_send(&p,section,sizeof section,true));
    assert(p.ring.host_index==2&&p.ring.card_index==2&&doors==2&&waits==1&&p.sent==2);
    assert(((u32)packet[3]<<24)==(14u<<24)&&packet[0]==sizeof section);
    assert(memcmp(packet+24,section,sizeof section)==0);
    p.failed=false;path_ready=false;
    assert(!be_send(&p,header,sizeof header,false)&&p.failed&&waits==2);
    p.failed=false;p.expect_fwdl_path=false;
    assert(be_send(&p,header,sizeof header,false)&&waits==2);
    /* Exercise actual ring_add/free for more than two complete wraps.  A
     * cached card_index left at zero fails around packet 127. */
    u8 chunk[2020]={0};
    for(int i=0;i<320;i++){
        assert(be_send(&p,chunk,sizeof chunk,true));
        assert(p.ring.card_index==p.ring.host_index);
        assert(rtw89_ring_free(&p.ring)==127);
    }
    assert(p.ring.host_index==(u16)((4+320)%128));
    /* Timeout must leave the shared backing unreusable even if a late card
     * completion arrives. Caller persists p.failed as fwdl_ring_poisoned. */
    complete=false;
    assert(!be_send(&p,chunk,sizeof chunk,true)&&p.failed&&delay_calls==100);
    u32 before=doors;hardware_done=p.ring.host_index;
    assert(!be_send(&p,chunk,sizeof chunk,true)&&doors==before);
    puts("PASS RTL8922AE CH12 firmware transport: real BD ring 320 packets/wrap, H2C/FWDL phases, timeout reuse refusal");
}
'''
download = function(src, 'be_download_firmware')
assert 'if (!card || card->fwdl_ring_poisoned) return false;' in download
assert 'card->fwdl_ring_poisoned = true;' in download[download.index('if (!rtw89_fw_download('):]
assert download.index('R_BE_SECURE_BOOT_MALLOC_INFO') < download.index('rtw89_fwdl_path_ready(regs, true') < download.index('rtw89_fw_download(')
assert 'if (!bbmcu0)' in download
assert '0x20248000u' in download
assert 'rtw89_fw_wait_running(regs, 2000, dev->model)' in src
run_test(code, 'rtw89-fwdl-transport')
