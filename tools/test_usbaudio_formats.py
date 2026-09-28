#!/usr/bin/env python3
"""Execute actual UAC1/UAC2 parser and endpoint selection on modeled descriptors.

Layouts independently follow Linux's USB specification structs:
https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/usb/audio.h
https://raw.githubusercontent.com/torvalds/linux/master/include/linux/usb/audio-v2.h
These are spec-shaped fixtures, NOT captures or proof of audible hardware output.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT / 'kernel/usbaudio.c').read_text()
    usb = (ROOT / 'kernel/usb.h').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef struct { int unused; } usb_device_t;
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
'''
    for text, names in ((src, ('USB_CLASS_AUDIO', 'AUDIO_SUBCLASS_CONTROL',
            'AUDIO_SUBCLASS_STREAM', 'AUDIO_PROTOCOL_V2', 'AUDIO_DT_INTERFACE',
            'AS_GENERAL', 'AS_FORMAT_TYPE', 'MAX_ALTERNATES', 'MAX_AUDIO_DEVICES')),
            (usb, ('USB_DT_INTERFACE', 'USB_DT_ENDPOINT', 'USB_EP_XFER_MASK',
                   'USB_EP_XFER_ISOC', 'USB_DIR_IN'))):
        for name in names:
            code += re.search(r'^#define\s+' + name + r'\s+[^\n]+', text, re.M)[0] + '\n'
    code += src[src.index('typedef struct {'):src.index('static audio_dev_t devices')]
    code += 'static audio_dev_t devices[MAX_AUDIO_DEVICES];\n'
    for name in ('audio_alt_valid', 'read_alternates', 'choose_best',
                 'usbaudio_next_endpoint', 'usbaudio_endpoint_open'):
        code += function(src, name) + '\n'
    ring = src.index('#define RING_BYTES')
    code += src[ring:src.index('/* ------------------------------------------------------------------- tests', ring)]
    code += function(src, 'usbaudio_ring_selftest') + '\n'
    # Execute the actual USB ioctl branch; the unrelated HDA switch is outside
    # this harness and replaced by a sentinel, not a model of USB format logic.
    hda = (ROOT / 'kernel/hda.c').read_text()
    ioctl = function(hda, 'audio_dev_ioctl')
    code += '#define E_INVAL 22\n#define E_NODEV 19\nstatic const char usb_audio_marker;\n'
    code += ioctl[:ioctl.index('    switch (cmd)')] + '    return -777;\n}\n'
    code += r'''
static u8 cfg[4096]; static int used;
static void append(const u8 *p, int n) { assert(used+n<=(int)sizeof cfg); memcpy(cfg+used,p,n); used+=n; }
static void iface(u8 num, u8 alt, u8 proto, u8 sub) {
    u8 d[]={9,4,num,alt,1,1,sub,proto,0}; append(d,sizeof d);
}
static void ep(u8 addr,u8 attrs) {
    u8 d[]={7,5,addr,attrs,0x40,2,1}; append(d,sizeof d);
}
static void alt(u8 proto,u8 num,u8 setting,u8 channels,u8 bytes,u8 bits,u8 addr) {
    iface(num,setting,proto,2);
    if(proto==0x20) {
        u8 g[]={16,0x24,1,1,0,1,1,0,0,0,channels,3,0,0,0,0};
        u8 f[]={6,0x24,2,1,bytes,bits}; append(g,sizeof g); append(f,sizeof f);
    } else {
        u8 g[]={7,0x24,1,1,0,1,0};
        u8 f[]={11,0x24,2,1,channels,bytes,bits,1,0x80,0xbb,0};
        append(g,sizeof g); append(f,sizeof f);
    }
    ep(addr,1);
}
static audio_dev_t parse(int n) {
    audio_dev_t a={.used=true}; read_alternates(&a,cfg,n); choose_best(&a); return a;
}
static void open_selected(audio_dev_t *a,int expected_out,int expected_in) {
    u8 addr,interval,burst,ifc,setting; u16 packet;
    for(int pass=0;pass<2;pass++) {
        int i=pass?expected_in:expected_out;
        if(i<0) continue;
        assert(usbaudio_next_endpoint(a,&addr,&packet,&interval,&burst,&ifc,&setting));
        assert(addr==a->alt[i].endpoint && setting==a->alt[i].alternate);
        assert(ifc==a->alt[i].interface && packet==a->alt[i].max_packet);
        assert(interval==1 && burst==0);
        usbaudio_endpoint_open(a,addr,true);
        assert(a->alt[i].opened);
        for(int j=0;j<a->alt_count;j++) if(j!=expected_out && j!=expected_in)
            assert(!a->alt[j].opened);
    }
    assert(!usbaudio_next_endpoint(a,&addr,&packet,&interval,&burst,&ifc,&setting));
}
int main(void) {
    /* UAC1: headset shape, repeated playback endpoint at 16/24 bits. */
    used=0; iface(0,0,0,1);
    alt(0,1,1,2,2,16,1); alt(0,1,2,2,3,24,1); alt(0,2,1,1,2,16,0x81);
    audio_dev_t a=parse(used);
    assert(!a.version2 && a.alt_count==3 && a.best_out==1 && a.best_in==2);
    assert(a.alt[1].channels==2 && a.alt[1].bits==24 && a.alt[1].bytes_per_sample==3);
    assert(a.alt[2].channels==1 && a.alt[2].bits==16);
    usbaudio_endpoint_open(&a,1,true); assert(!a.alt[0].opened && !a.alt[1].opened);
    assert(a.play_out==0 && a.best_out==1); open_selected(&a,0,2);
    /* UAC2: QuadCast-shaped five settings, not a 16-bit open despite address reuse. */
    used=0; iface(0,0,0x20,1);
    alt(0x20,1,1,2,2,16,0x81); alt(0x20,1,2,2,3,24,0x81);
    alt(0x20,1,3,2,4,32,0x81); alt(0x20,2,1,2,2,16,4);
    alt(0x20,2,2,2,3,24,4); ep(0x84,0x11); /* explicit feedback, not input audio */
    a=parse(used); assert(a.version2 && a.alt_count==5 && a.best_in==2 && a.best_out==4);
    assert(a.alt[4].endpoint==4 && !a.alt[4].input && a.play_out==-1);
    open_selected(&a,-1,2);
    /* Failure updates the offered alternate only; no false playable endpoint. */
    a=parse(used); a.alt[4].offered=true; usbaudio_endpoint_open(&a,4,false);
    for(int i=0;i<a.alt_count;i++) assert(!a.alt[i].opened);
    /* Per-stream protocol, not whichever AudioControl interface happened last. */
    used=0; iface(0,0,0x20,1); alt(0,1,1,1,2,16,0x83);
    a=parse(used); assert(a.alt_count==1 && a.alt[0].channels==1 && a.alt[0].bits==16);
    unsigned checks=0;
    for(int v=0;v<2;v++) for(int channels=0;channels<=8;channels++)
    for(int bytes=0;bytes<=5;bytes++) for(int bits=0;bits<=40;bits++) {
        used=0; alt(v?0x20:0,1,1,channels,bytes,bits,1); a=parse(used);
        bool valid=channels && bytes && bytes<=4 && bits && bits<=8*bytes;
        assert(a.alt_count==(int)valid);
        if(valid) assert(a.alt[0].bits==bits && a.alt[0].channels==channels &&
                         a.alt[0].bytes_per_sample==bytes);
        checks++;
    }
    for(int v=0;v<2;v++) {
        used=0; alt(v?0x20:0,1,1,2,4,24,1); int full=used;
        for(int n=0;n<full;n++) { a=parse(n); assert(a.alt_count==0); checks++; }
        /* Unsupported format type, missing/unsupported PCM format, missing channels. */
        int general=9, format=v?25:16;
        cfg[format+3]=2; assert(parse(full).alt_count==0); cfg[format+3]=1;
        cfg[general+(v?6:5)]=0; assert(parse(full).alt_count==0);
        cfg[general+(v?6:5)]=1;
        cfg[7]=0x30; assert(parse(full).alt_count==0); cfg[7]=v?0x20:0;
        cfg[full-4]=0x11; assert(parse(full).alt_count==0); cfg[full-4]=1;
        cfg[full-1]=0; assert(parse(full).alt_count==0);
    }
    /* UAC1 continuous range and truncated discrete list cannot read next descriptor. */
    used=0; iface(1,1,0,2);
    const u8 g[]={7,0x24,1,1,0,1,0}; append(g,sizeof g);
    const u8 continuous[]={14,0x24,2,1,2,3,24,0,0x80,0xbb,0,0,0x77,1};
    append(continuous,sizeof continuous); ep(1,1);
    assert(parse(used).alt_count==1);
    cfg[16+7]=3; assert(parse(used).alt_count==0);
    /* Fixed capacity and malformed descriptor length cannot overrun alt array. */
    used=0; for(int i=0;i<20;i++) alt(0x20,1,i+1,2,4,32,1);
    assert(parse(used).alt_count==MAX_ALTERNATES);
    cfg[0]=0; assert(parse(used).alt_count==0);
    /* Actual writer/drain gates: no raw s16 reaches 24/32-bit or unnegotiated
     * endpoints. Preserve bytes until a supported, opened device owns them. */
    used=0; alt(0,1,1,2,2,16,1); alt(0,1,2,2,3,24,1);
    devices[0]=parse(used); devices[0].alt[0].offered=true;
    usbaudio_endpoint_open(&devices[0],1,true);
    assert(usbaudio_can_play());
    u32 rate=0,ch=0,bits=0; assert(usbaudio_playback_format(&rate,&ch,&bits));
    assert(rate==48000 && ch==2 && bits==16 && devices[0].alt[devices[0].best_out].bits==24);
    u32 active[3]={0}; assert(!audio_dev_ioctl((void *)&usb_audio_marker,3,active));
    assert(active[0]==48000 && active[1]==2 && active[2]==16);
    assert(audio_dev_ioctl((void *)&usb_audio_marker,3,NULL)==-E_INVAL);
    assert(audio_dev_ioctl(NULL,3,active)==-777);
    const u8 samples[]={1,2,3,4,5,6,7,8}; u8 output[12]; memset(output,0xa5,sizeof output);
    assert(usbaudio_write(samples,3)==3);
    assert(!usbaudio_next_samples(&devices[0],1,output,12));
    assert(usbaudio_write(samples+3,5)==5);
    assert(!usbaudio_next_samples(&devices[0],1,output,3));
    assert(!usbaudio_next_samples(&devices[0],0x81,output,12));
    devices[1]=devices[0];
    assert(!usbaudio_next_samples(&devices[1],1,output,12));
    memset(&devices[1],0,sizeof devices[1]);
    audio_dev_t good=devices[0];
    for(int f=0;f<7;f++) {
        devices[0]=good;
        if(f==0) devices[0].alt[0].bits=24;
        if(f==1) devices[0].alt[0].bits=32;
        if(f==2) devices[0].alt[0].bytes_per_sample=4;
        if(f==3) devices[0].alt[0].channels=1;
        if(f==4) devices[0].alt[0].fixed_48k=false;
        if(f==5) devices[0].alt[0].opened=false;
        if(f==6) devices[0].play_out=1;
        u32 head=playback_ring.head,tail=playback_ring.tail;
        assert(!usbaudio_can_play() && !usbaudio_write(samples,8));
        assert(!usbaudio_next_samples(&devices[0],1,output,12));
        assert(!usbaudio_playback_format(&rate,&ch,&bits));
        assert(audio_dev_ioctl((void *)&usb_audio_marker,3,active)==-E_NODEV);
        assert(playback_ring.head==head && playback_ring.tail==tail);
    }
    devices[0]=good;
    assert(usbaudio_next_samples(&devices[0],1,output,7)==4);
    assert(usbaudio_next_samples(&devices[0],1,output+4,8)==4);
    assert(!memcmp(output,samples,8) && output[8]==0xa5);
    assert(usbaudio_write(samples,8)==8);
    static audio_ring_t before_selftest;
    memcpy(&before_selftest,&playback_ring,sizeof playback_ring);
    assert(!usbaudio_ring_selftest());
    assert(!memcmp(&before_selftest,&playback_ring,sizeof playback_ring));
    assert(usbaudio_next_samples(&devices[0],1,output,12)==8);
    assert(!memcmp(samples,output,8));
    assert(!memcmp(&devices[0],&good,sizeof good));
    /* Merely advertising 48k among a range is not clock negotiation. */
    used=0; alt(0,1,1,2,2,16,1); cfg[16+8]=0x44; cfg[16+9]=0xac;
    assert(parse(used).play_out==-1); /* fixed 44100, not 48000 */
    printf("PASS actual UAC1/UAC2 parser: %u field/truncation cases; endpoint alternate identity and feedback filtering\n",checks);
}
'''
    run_test(code, 'usbaudio_formats')


if __name__ == '__main__':
    main()
