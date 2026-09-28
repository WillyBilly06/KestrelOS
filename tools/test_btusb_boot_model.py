#!/usr/bin/env python3
"""Test the actual boot model, not a replacement model of that model."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    model=(ROOT/'kernel/btusb_model.c').read_text()
    bt=(ROOT/'kernel/btusb.c').read_text()
    model='\n'.join(line for line in model.splitlines() if not line.startswith('#include'))
    end=bt.index('} bt_t;')+len('} bt_t;')
    record=bt[bt.rfind('typedef struct {',0,end):end]
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef int8_t s8;
typedef void usb_device_t;
static u64 g_uptime_ms;
static void usb_poll_device_events(void *d){(void)d;assert(0);}
static void sched_sleep_ms(int n){g_uptime_ms+=n;}
'''+record+r'''
static unsigned events;
void btusb_event(void *ctx,const u8 *data,int len){
    bt_t *b=ctx;assert(len>=2 && data[1]==len-2 && len<=(int)sizeof b->event);
    memcpy(b->event,data,len);b->event_len=len;b->event_waiting=true;events++;
}
'''+model+'\n#define HCI_EV_COMMAND_COMPLETE 0x0e\n'+function(bt,'wait_for_complete')+r'''
int main(void){
    bt_t b={.used=true,.modelled=true};btusb_model_attach();
    const u16 ops[]={HCI_RESET,HCI_READ_LOCAL_VER,HCI_READ_BD_ADDR};
    for(int i=0;i<3;i++){
        u8 packet[]={ops[i]&255,ops[i]>>8,0};
        btusb_model_command(&b,packet,3);u8 result[16]={0};int n=sizeof result;
        assert(wait_for_complete(&b,ops[i],result,&n,100));
        if(i==0)assert(n==0);
        if(i==1)assert(n==8&&result[4]==15);
        if(i==2)assert(n==6&&result[0]==0xff&&result[5]==0xaa);
    }
    assert(events==3&&btusb_model_commands()==3&&g_uptime_ms==0);
    u8 request[]={ATT_READ_BY_TYPE_REQ,1,0,0xff,0xff,3,0x28};
    btusb_model_attribute(4,request,sizeof request);
    u8 guarded[40];memset(guarded,0xa5,sizeof guarded);u16 cid=0;
    int n=btusb_model_attribute_reply(&cid,guarded+4,32);
    assert(n==23&&cid==4&&guarded[4]==ATT_READ_BY_TYPE_RSP&&guarded[5]==7);
    assert(guarded[6]==0x10&&guarded[13]==0x20&&guarded[20]==0x30);
    for(int i=0;i<4;i++)assert(guarded[i]==0xa5);
    for(int i=27;i<40;i++)assert(guarded[i]==0xa5);
    puts("BTUSB_BOOT_MODEL_PASS real completion framing, strict driver waiter, 3 bounded attribute records");
}
'''
    run_test(code,'btusb_boot_model')


if __name__=='__main__':
    main()
