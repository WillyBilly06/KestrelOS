#!/usr/bin/env python3
"""Actual Bluetooth startup/wait/event and xHCI pump, with modeled USB edges.

The asynchronous radio model answers ONLY when the real pump drains its event;
sleeping does not inject a reply. Also checks real waiter routing and guarded
class polling. Does not prove Realtek firmware readiness or native radio I/O.
Command setup reference: Linux drivers/bluetooth/btusb.c alloc_ctrl_urb and
btusb_probe (normal HCI, not AMP).
"""
import re
from test_gpu_stable_candidate import ROOT, run_test


def fn(src, name):
    m = re.search(r'^.*?\b' + name + r'\([^;]*?\)\s*\{', src, re.M)
    assert m, name
    return src[m.start():src.index('\n}', m.end()) + 2] + '\n'


def struct(src, name):
    end = src.index('} ' + name + ';') + len('} ' + name + ';')
    return src[src.rfind('typedef struct {', 0, end):end] + '\n'


def main():
    bt = (ROOT / 'kernel/btusb.c').read_text()
    xhci = (ROOT / 'kernel/xhci.c').read_text()
    usb = (ROOT / 'kernel/usb.h').read_text()
    # The circular dependency is real: enumeration runs on the outer pump thread.
    thread = fn(xhci, 'usb_thread')
    assert thread.index('scan_ports(x)') < thread.index('for (;;)')
    assert 'btusb_start(drv)' in fn(xhci, 'offer')
    assert 'bind_interfaces(dev' in fn(xhci, 'enumerate_at')
    poll = fn(xhci, 'usb_poll_device_events')
    assert 'scan_ports(' not in poll and 'scan_hubs(' not in poll
    assert 'process_pending_resets(' not in poll and 'pump(x, 0, NULL, 0)' in poll
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16;
typedef uint32_t u32; typedef uint64_t u64;
#define ARRAY_LEN(x) (sizeof(x)/sizeof((x)[0]))
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
#define kerr(...) ((void)0)
#define pause_cpu() assert(0)
typedef struct { u64 param; u32 status,control; } trb_t;
typedef struct xhci {
    bool controller_up;
    u32 class_poll_active,events_seen,transfer_events;
    u8 last_transfer_cc;
    struct xfer_waiter { volatile u64 token; volatile u32 ready; trb_t ev; } waiters[16];
    volatile u32 waiters_lock;
} xhci_t;
typedef struct { bool present; xhci_t *hc; } usb_device_t;
static u64 g_uptime_ms;
static void sched_sleep_ms(int ms) { assert(ms>0); g_uptime_ms+=ms; }
static void strlcpy(char *out,const char *in,size_t cap) { snprintf(out,cap,"%s",in); }
'''
    for src, names in ((usb, ('USB_DIR_OUT', 'USB_TYPE_CLASS', 'USB_RECIP_DEVICE')),
                       (xhci, ('TRB_EV_TRANSFER', 'TRB_EV_CMD', 'TRB_EV_PORT', 'TRB_EV_HC'))):
        for name in names:
            code += re.search(r'^#define\s+' + name + r'\s+[^\n]+', src, re.M)[0] + '\n'
    code += '\n'.join(re.findall(r'^#define HCI_[^\n]+', bt, re.M)) + '\n'
    code += struct(bt, 'bt_t') + struct(bt, 'bt_key_t') + struct(bt, 'bt_link_t')
    code += r'''
#define MAX_KEYS 8
static bt_key_t keys[MAX_KEYS];
static bt_t *pairing_adapter;
static bool pairing_done,pairing_ok;
static bt_link_t link;
static bt_key_t *key_for(const u8 *addr) { (void)addr; return NULL; }
static void keys_save(void) {}
static void keys_load(void) {}
static void inquiry_result(const u8 *p,int n,u8 code) {(void)p;(void)n;(void)code;}
static void advertising_report(const u8 *p,int n) {(void)p;(void)n;}
static int usb_control(usb_device_t *,u8,u8,u16,u16,void *,u16);
static void btusb_model_command(void *,const u8 *,int);
void btusb_event(void *,const u8 *,int);
void usb_poll_device_events(usb_device_t *);
static int popped,acks,ports,transfers;
static bt_t *target;
typedef struct { trb_t trb; u8 data[260]; int len; u64 at; } frame;
static frame queue[32],current_frame;
static int qhead,qtail;
static void enqueue(u64 token,u32 type,const u8 *p,int n,u64 at) {
    assert(qtail<(int)ARRAY_LEN(queue)); int i=qtail++;
    while(i>qhead && queue[i-1].at>at) {queue[i]=queue[i-1];i--;}
    frame *f=&queue[i]; memset(f,0,sizeof *f);
    f->trb=(trb_t){token,1u<<24,type<<10}; f->at=at; f->len=n;
    if(n) memcpy(f->data,p,n);
}
static bool event_pop(xhci_t *x,trb_t *e) {
    assert(!x->waiters_lock);
    if(qhead==qtail || queue[qhead].at>g_uptime_ms) return false;
    current_frame=queue[qhead++]; *e=current_frame.trb; popped++; return true;
}
static void event_ack(xhci_t *x) {assert(!x->waiters_lock);acks++;}
static void handle_transfer(xhci_t *x,const trb_t *e) {
    assert(!x->waiters_lock && e->param==1); transfers++;
    btusb_event(target,current_frame.data,current_frame.len);
}
static void handle_port_change(xhci_t *x,const trb_t *e) {
    (void)e; assert(!x->waiters_lock); ports++;
    int before=popped; usb_poll_device_events(target->dev); assert(popped==before);
}
'''
    for name in ('waiters_acquire', 'waiters_release_lock', 'waiter_register',
                 'waiter_unregister', 'waiter_deliver', 'pump', 'usb_poll_device_events'):
        code += fn(xhci, name)
    for name in ('send_command', 'wait_for_complete', 'addr_from_wire',
                 'btusb_event', 'manufacturer_name', 'btusb_start'):
        code += fn(bt, name)
    code += r'''
static unsigned commands,delay_ms=7;
static int fault; /* 1 no reply,2 error status,3 failed control,4 truncation,5 wrong opcode */
static u8 reply[32]; static int reply_len;
static void make_reply(u16 op) {
    const u8 ver[]={13,0x34,0x12,13,0x5d,0,0,0};
    const u8 addr[]={6,5,4,3,2,1};
    memset(reply,0,sizeof reply); reply[0]=0x0e; reply[1]=4; reply[2]=1;
    reply[3]=(u8)op; reply[4]=(u8)(op>>8); reply_len=6;
    if(op==HCI_READ_LOCAL_VER) {memcpy(reply+6,ver,sizeof ver);reply[1]+=sizeof ver;reply_len+=sizeof ver;}
    if(op==HCI_READ_BD_ADDR) {memcpy(reply+6,addr,sizeof addr);reply[1]+=sizeof addr;reply_len+=sizeof addr;}
    if(fault==2) reply[5]=0x0c;
    if(fault==4) reply[1]++;
    if(fault==5) reply[3]^=1;
}
static int usb_control(usb_device_t *d,u8 type,u8 req,u16 value,u16 index,void *buf,u16 len) {
    assert(d==target->dev && d->present && type==0x20 && req==0 && !value && !index);
    assert(len>=3); const u8 *p=buf; assert(p[2]+3==len); commands++;
    if(fault==3) return -1;
    make_reply(p[0]|((u16)p[1]<<8));
    if(fault!=1) enqueue(1,TRB_EV_TRANSFER,reply,reply_len,g_uptime_ms+delay_ms);
    /* A real control send also pumps to await its own USB transfer token.
     * This is essential when send_command is called INSIDE btusb_event. */
    enqueue(0xcafe,TRB_EV_TRANSFER,NULL,0,g_uptime_ms);
    trb_t completion; assert(pump(d->hc,0xcafe,&completion,10)==1);
    return len;
}
static void btusb_model_command(void *ctx,const u8 *p,int len) {
    assert(ctx==target && len==3); commands++; make_reply(p[0]|((u16)p[1]<<8));
    btusb_event(ctx,reply,reply_len);
}
static xhci_t hc,other;
static usb_device_t dev;
static bt_t adapter;
static void reset(int f) {
    memset(&hc,0,sizeof hc); hc.controller_up=true;
    memset(&other,0,sizeof other); other.controller_up=true;
    dev=(usb_device_t){true,&hc}; adapter=(bt_t){.used=true,.dev=&dev,.interface=7};
    target=&adapter; qhead=qtail=0; popped=acks=ports=transfers=0;
    g_uptime_ms=100; fault=f; commands=0; delay_ms=7;
    pairing_adapter=NULL;
}
int main(void) {
    reset(0); btusb_start(&adapter);
    assert(adapter.reset_done && commands==3 && transfers==3 && g_uptime_ms==121);
    assert(adapter.hci_version==13 && adapter.hci_revision==0x1234 && adapter.manufacturer==0x5d);
    const u8 address[]={1,2,3,4,5,6}; assert(!memcmp(adapter.bd_addr,address,6));
    assert(!strcmp(adapter.maker,"Realtek") && !other.events_seen && !hc.class_poll_active);
    for(int f=1;f<=5;f++) {
        reset(f); btusb_start(&adapter); assert(!adapter.reset_done && commands==1);
        assert(g_uptime_ms<=1100 && !hc.class_poll_active);
        if(f==1 || f==4 || f==5) assert(g_uptime_ms==1100);
        if(f==2) assert(g_uptime_ms==107);
        if(f==3) assert(g_uptime_ms==100);
    }
    reset(0); adapter.modelled=true; btusb_start(&adapter);
    assert(adapter.reset_done && commands==3 && !popped && g_uptime_ms==100);
    /* Cached unrelated opcode cannot pass; a subsequent matching event can. */
    reset(0); make_reply(HCI_READ_BD_ADDR); btusb_event(&adapter,reply,reply_len);
    make_reply(HCI_RESET); enqueue(1,TRB_EV_TRANSFER,reply,reply_len,103);
    assert(wait_for_complete(&adapter,HCI_RESET,NULL,NULL,20) && g_uptime_ms==103);
    /* USB padding is NOT an extra HCI parameter; cap and guard bytes preserved. */
    reset(0); make_reply(HCI_READ_BD_ADDR); memset(reply+reply_len,0xee,4);
    btusb_event(&adapter,reply,reply_len+4); u8 out[12]; memset(out,0xa5,sizeof out);
    int n=3; assert(wait_for_complete(&adapter,HCI_READ_BD_ADDR,out+2,&n,10) && n==3);
    assert(out[0]==0xa5 && out[1]==0xa5 && out[5]==0xa5 && out[2]==6 && out[4]==4);
    reset(0); make_reply(HCI_RESET); reply[1]=3; btusb_event(&adapter,reply,6);
    assert(!wait_for_complete(&adapter,HCI_RESET,NULL,NULL,4) && g_uptime_ms==104);
    /* Declared timeout remains bounded; no retry, recursive scan or second controller. */
    reset(0); assert(!wait_for_complete(&adapter,HCI_RESET,NULL,NULL,0)); assert(!popped);
    assert(!wait_for_complete(NULL,HCI_RESET,NULL,NULL,10));
    adapter.used=false; assert(!wait_for_complete(&adapter,HCI_RESET,NULL,NULL,10));
    /* Wrapper recursion skips itself. An unrelated registered transfer is delivered
     * to its waiter, not the Bluetooth callback, while this thread polls. */
    reset(0); struct xfer_waiter *w=waiter_register(&hc,0xbeef); assert(w);
    enqueue(0xbeef,TRB_EV_TRANSFER,NULL,0,100); enqueue(0,TRB_EV_PORT,NULL,0,100);
    make_reply(HCI_RESET); enqueue(1,TRB_EV_TRANSFER,reply,reply_len,100);
    usb_poll_device_events(&dev);
    assert(w->ready && w->ev.param==0xbeef && ports==1 && transfers==1 && popped==3);
    assert(!hc.class_poll_active && !hc.waiters_lock && acks==1);
    waiter_unregister(&hc,w); assert(!w->token);
    assert(wait_for_complete(&adapter,HCI_RESET,NULL,NULL,1));
    /* Actual IO-capability event -> send_command -> usb_control -> nested pump.
     * The wrapper's anti-recursion guard must NOT block a synchronous control
     * completion; the later HCI completion remains available to the caller. */
    reset(0); pairing_adapter=&adapter;
    const u8 cap_request[]={HCI_EV_IO_CAP_REQUEST,6,6,5,4,3,2,1};
    enqueue(1,TRB_EV_TRANSFER,cap_request,sizeof cap_request,100);
    usb_poll_device_events(&dev);
    assert(commands==1 && popped==2 && transfers==1 && g_uptime_ms==100);
    assert(!hc.class_poll_active && !hc.waiters_lock);
    for(unsigned i=0;i<ARRAY_LEN(hc.waiters);i++) assert(!hc.waiters[i].token);
    assert(wait_for_complete(&adapter,HCI_IO_CAP_REPLY,NULL,NULL,20));
    assert(g_uptime_ms==107 && transfers==2);
    int before=popped; usb_poll_device_events(NULL); dev.present=false; usb_poll_device_events(&dev);
    dev.present=true; hc.controller_up=false; usb_poll_device_events(&dev);
    hc.controller_up=true; dev.hc=NULL; usb_poll_device_events(&dev); assert(popped==before);
    puts("PASS actual Bluetooth startup/event/wait and xHCI pump: async delivery, exact class-device setup, failure bounds, opcode/length/capacity checks, token routing and reentry guards");
}
'''
    run_test(code, 'btusb_startup')


if __name__ == '__main__':
    main()
