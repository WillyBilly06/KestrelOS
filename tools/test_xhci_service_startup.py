#!/usr/bin/env python3
"""Execute the production USB thread with idle devices and controller faults."""
import re
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT / 'kernel/xhci.c').read_text()
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
typedef uint32_t u32; typedef uint64_t u64;
#define MAX_DEVICES 2
typedef struct { bool active,polled; unsigned completions,dci; } usb_endpoint_t;
typedef struct { bool present; int ep_count; usb_endpoint_t ep[2]; void *dev_ctx; } usb_device_t;
typedef struct {
    unsigned bus,slot,func,max_ports; bool initial_scan_done,controller_up;
    void *op_regs; usb_device_t devices[MAX_DEVICES];
    u32 events_seen,transfer_events,last_transfer_cc,beat_events,beat_transfers;
    u32 beat_completions,beat_endpoints; u64 beat_forced_at;
} xhci_t;
static u64 g_uptime_ms;
static unsigned keyboards,mice,stage,iterations,error_count;
static bool reset_ok=true;
static xhci_t *current;
static jmp_buf end;
static char errors[8][200];
static void kerr(const char *tag,const char *fmt,...) {
    (void)tag; assert(error_count<8);va_list ap;va_start(ap,fmt);
    vsnprintf(errors[error_count++],200,fmt,ap);va_end(ap);
}
#define kinfo(...) ((void)0)
#define kwarn(...) assert(0)
static void bios_handoff(xhci_t *x){(void)x;}
static bool reset_controller(xhci_t *x){(void)x;return reset_ok;}
static bool setup_memory(xhci_t *x){(void)x;return true;}
static u32 portsc(xhci_t *x,u32 p){(void)x;(void)p;return 0;}
static u32 portsc_keep(u32 v){return v;}
static void portsc_write(xhci_t *x,u32 p,u32 v){(void)x;(void)p;(void)v;}
static void scan_ports(xhci_t *x){(void)x;}
static void scan_hubs(xhci_t *x){(void)x;}
static void report_ports(xhci_t *x){(void)x;}
static void pump(xhci_t *x,int a,void *b,int c){
    (void)a;(void)b;(void)c;assert(x->initial_scan_done&&stage==0);stage=1;
}
static void process_pending_resets(xhci_t *x){(void)x;assert(stage==1);stage=2;}
static void usbhid_tick(void){assert(stage==2);stage=3;}
static void pump_isoch(xhci_t *x){(void)x;assert(stage==3);stage=4;}
static void sched_sleep_ms(int ms){
    if(ms==100){assert(!current->initial_scan_done);g_uptime_ms+=ms;return;}
    assert(ms==1&&stage==4);stage=0;
    if(!iterations)assert(g_uptime_ms==100); /* no wait for user input */
    iterations++;g_uptime_ms+=ms;if(iterations==14001)longjmp(end,1);
}
static u32 *ep_ctx(xhci_t *x,void *c,unsigned dci,bool input){
    (void)x;(void)c;(void)dci;(void)input;static u32 words[]={1,0,0,0};return words;
}
static void wr32(void *base,u32 reg,u32 value){(void)base;(void)reg;(void)value;}
'''
    for name in ('PORTSC_PP', 'OP_USBSTS', 'USBSTS_HCH', 'USBSTS_HSE',
                 'USBSTS_CNR', 'USBSTS_HCE', 'USBSTS_PCD'):
        code += re.search(r'^#define\s+' + name + r'\s+[^\n]+',src,re.M)[0]+'\n'
    code += r'''
static u32 rd32(void *base,u32 reg){
    (void)base;assert(reg==OP_USBSTS);
    if(g_uptime_ms<4000)return USBSTS_PCD; /* idle controller is healthy */
    if(g_uptime_ms<8000)return USBSTS_HCH|USBSTS_HSE;
    if(g_uptime_ms<10000)return USBSTS_CNR;
    if(g_uptime_ms<12000)return USBSTS_HCE;
    return 0;
}
'''+function(src,'usb_thread')+r'''
int main(void){
    for(int hid=0;hid<2;hid++){
        xhci_t x={0};current=&x;keyboards=hid;mice=hid;
        g_uptime_ms=iterations=stage=error_count=0;
        if(hid){x.devices[0].present=true;x.devices[0].ep_count=1;
            x.devices[0].ep[0].active=x.devices[0].ep[0].polled=true;}
        if(!setjmp(end))usb_thread(&x);
        assert(iterations==14001&&x.initial_scan_done&&x.controller_up);
        assert(error_count==3); /* once per actual fault change, never idle */
        assert(strstr(errors[0],"HALTED")&&strstr(errors[0],"HOST-SYSTEM-ERROR"));
        assert(strstr(errors[1],"NOT-READY")&&!strstr(errors[1],"HOST-CONTROLLER"));
        assert(strstr(errors[2],"HOST-CONTROLLER-ERROR")&&!strstr(errors[2],"NOT-READY"));
    }
    reset_ok=false;xhci_t x={0};error_count=0;usb_thread(&x);
    assert(x.initial_scan_done&&!x.controller_up&&error_count==1);
    puts("XHCI_SERVICE_STARTUP_PASS idle HID/no HID: immediate reset/HID/isoch service, actual status fault transitions");
}
'''
    run_test(code, 'xhci_service_startup')


if __name__ == '__main__':
    main()
