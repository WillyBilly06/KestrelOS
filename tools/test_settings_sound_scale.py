#!/usr/bin/env python3
"""Execute production scale policy, Sound layout/selection and HDA mute code.

Driver calls and drawing are modeled boundaries, not audible hardware proof.
"""
from test_gpu_stable_candidate import ROOT, function, run_test
import re


def main():
    draw = (ROOT / 'user/libgui/draw.c').read_text()
    code = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
static int ui_scale=1,scale_override;
static bool ui_scale_explicit;
'''
    code += draw[draw.index('static int eff_scale'):draw.index('theme_t g_theme;')]
    code += '''
int main(void){
    for(int w=0;w<32000;w+=79)assert(gui_scale_for_width(w)==1);
    gui_set_default_scale(1);assert(gui_scale()==1);
    gui_set_scale(2);gui_set_default_scale(1);assert(gui_scale()==2);
    int old=gui_scale_push(3);assert(gui_scale()==3&&gui_screen_scale()==2);
    gui_scale_pop(old);assert(gui_scale()==2);
    gui_set_scale(-100);assert(gui_scale()==1);
    gui_set_scale(100);gui_set_default_scale(1);assert(gui_scale()==3);
    puts("SCALE_POLICY_PASS compact default; explicit choice survives display startup");
}
'''
    run_test(code, 'settings_scale')

    hda = (ROOT / 'kernel/hda.c').read_text()
    code = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
static bool ready=true,out_muted;
static u8 sel_codec,dac_node=2,out_pin_node=3;
static int calls,fail;
#define VERB_SET_AMP_GAIN 3
#define VERB16(c,n,v,p) (((u32)(c)<<28)|((u32)(n)<<20)|((u32)(v)<<16)|(p))
static u8 max_gain(u8 c,u8 n,bool out){assert(c==sel_codec&&n&&out);return 42;}
static bool codec_command(u32 verb,u32 *response){
    assert(!response && (verb>>28)==sel_codec);return ++calls!=fail;
}
'''
    code += function(hda, 'set_output_mute')
    code += '''
int main(void){
    assert(set_output_mute(true)&&out_muted&&calls==2);
    assert(set_output_mute(false)&&!out_muted&&calls==4);
    calls=0;fail=1;assert(!set_output_mute(true)&&!out_muted&&calls==1);
    calls=0;fail=2;assert(!set_output_mute(true)&&!out_muted&&calls==2);
    ready=false;calls=0;assert(!set_output_mute(true)&&!calls);
    ready=true;dac_node=0;assert(!set_output_mute(true)&&!calls);
    puts("HDA_MUTE_PASS codec zero and command failure propagation (modeled transport)");
}
'''
    run_test(code, 'settings_hda_mute')

    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
static u32 *corb;static u64 *rirb,corb_phys,rirb_phys;
static u16 corb_write_pos,rirb_read_pos;
static unsigned regs[256],pending,calls;
static u64 pages[2][512];static int allocations;
static void *dma_alloc_pages(int n,u64 *p){assert(n==1&&allocations<2);*p=0x1000+0x1000*allocations;return pages[allocations++];}
static void timer_mdelay(int n){(void)n;}
static void timer_udelay(int n){(void)n;}
'''
    for name in ('CORB_ENTRIES','RIRB_ENTRIES','CORBCTL','RIRBCTL','CORBLBASE','CORBUBASE',
                 'CORBSIZE','CORBRP','CORBWP','RIRBLBASE','RIRBUBASE','RIRBSIZE','RIRBWP',
                 'RINTCNT','INTCTL','RIRBSTS','RIRBCTL_RUN','RIRBCTL_RESPONSE_STATUS','CORBCTL_RUN'):
        code += re.search(r'^#define ' + name + r'\s+[^\n]+',hda,re.M)[0] + '\n'
    code += r'''
static void wr32(int reg,u32 value){regs[reg]=value;}
static u16 rd16(int reg){return regs[reg];}
static void wr8(int reg,u8 value){
    if(reg==RIRBSTS){if(regs[reg]&value&1)pending=0;regs[reg]&=~value;}
    else regs[reg]=value;
}
static void wr16(int reg,u16 value){
    regs[reg]=reg==RIRBWP?0:value;
    if(reg==CORBWP&&(regs[CORBCTL]&2)){
        /* QEMU's RINTCNT backpressure; acknowledgement must clear a set latch. */
        if(pending==regs[RINTCNT])return;
        unsigned wp=(regs[RIRBWP]+1)&255;
        rirb[wp]=0xabc000+(++calls);regs[RIRBWP]=wp;pending++;
        if(regs[RIRBCTL]&1)regs[RIRBSTS]|=1;
    }
}
'''
    code += function(hda,'setup_rings') + '\n' + function(hda,'codec_command')
    code += r'''
int main(void){
    assert(setup_rings());assert(!regs[INTCTL]);
    for(unsigned i=0;i<600;i++){u32 reply=0;assert(codec_command(i+1,&reply));assert(reply==0xabc001+i);}
    assert(!pending&&calls==600);
    puts("HDA_RING_PASS 600 sequential responses across wrap; status acknowledgement and global IRQ mask");
}
'''
    run_test(code,'settings_hda_ring')
    assert 'VERB(codec, pin, VERB_SET_EAPD, 0x02)' in hda

    src = (ROOT / 'user/desktop/app_settings.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,w,h;} rect_t;
typedef struct {int width,height;} surface_t;
typedef struct {unsigned sample_rate;unsigned char present,output_ready,bits,channels;
                char controller[48],codec[48];} kaudioinfo_t;
typedef struct {
    kaudioinfo_t audio;bool have_audio,audio_muted;
    unsigned audio_rates[12];int audio_rate_count;
    uint8_t audio_depths[5];int audio_depth_count;
    struct {uint8_t node,device,is_output,current;} audio_eps[24];int audio_ep_count;
    rect_t rc_mute,rc_out[24],rc_in[24],rc_rate[12],rc_depth[5];
    int rc_out_n,rc_in_n,rc_rate_n,rc_depth_n,sound_scroll,sound_total;
    char status[200];
} settings_t;
static struct {int text_dim,text,window,window_border;} g_theme;
static int scale=1,selected_node=1,selection_error,confirm=1,malformed;
static int reads,rates_read,closes,opened,fail_open;
#define FONT_UI 0
#define O_WRONLY 1
static rect_t rect_make(int x,int y,int w,int h){return(rect_t){x,y,w,h};}
static bool rect_contains(rect_t r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;}
static int gui_scale(void){return scale;}
static int gui_font_height(int f){(void)f;return 24*scale;}
static int gui_text_width(int f,const char *s){(void)f;return (int)strlen(s)*12*scale;}
static rect_t body_rect(surface_t *s){return rect_make(24,40,s->width-48,s->height-80);}
static void gui_button(surface_t *s,rect_t r,const char *t,bool hover,bool active,bool enabled){
    (void)t;(void)hover;(void)active;(void)enabled;
    rect_t b=body_rect(s);assert(r.w>=0&&r.x>=b.x&&r.x+r.w<=b.x+b.w);
}
static void gui_text_clipped(surface_t*s,int f,int x,int y,int w,const char*t,int c){
    (void)s;(void)f;(void)x;(void)y;(void)t;(void)c;assert(w>=0);}
static int gui_section(surface_t*s,int x,int y,int w,const char*t){
    (void)s;(void)x;(void)w;(void)t;return y+24*scale+8;}
static int gui_page_header(surface_t*s,int x,int y,int w,const char*t,const char*d){
    (void)s;(void)x;(void)w;(void)t;(void)d;return y+48*scale+22;}
static void gui_hline(surface_t*s,int x,int y,int w,int c){(void)s;(void)x;(void)y;(void)w;(void)c;}
static void gui_fill(surface_t*s,rect_t b,int c){(void)s;(void)b;(void)c;}
static void gui_scrollbar(surface_t*s,rect_t b,int pos,int vis,int total){
    (void)s;(void)b;assert(pos>=0&&pos<=total-vis);}
static int open(const char*p,int m){assert(!strcmp(p,"/dev/audio")&&m==O_WRONLY);opened++;return fail_open?-1:4;}
static int close(int fd){assert(fd==4);closes++;return 0;}
static int ioctl(int fd,int cmd,void *ptr){
    assert(fd==4);uint32_t *a=ptr;
    if(cmd==10){reads++;a[0]=malformed?0xffffffffu:2;
        a[1]=1;a[2]=2;a[3]=1;a[4]=confirm&&selected_node==1;
        a[5]=2;a[6]=10;a[7]=0;a[8]=confirm&&selected_node==2;a[9]=0;return 0;}
    if(cmd==8){rates_read++;a[0]=1;a[1]=48000;a[13]=1;a[14]=16;return 0;}
    assert(cmd==11||cmd==12);if(selection_error)return -1;selected_node=*a;return 0;
}
static int enum_audio(kaudioinfo_t*a){a->present=1;return 0;}
static void say(settings_t*s,const char*t){snprintf(s->status,sizeof s->status,"%s",t);}
'''
    code += (ROOT / 'user/desktop/settings_appearance.h').read_text()
    # Label function has a pointer return, so use its bounded source section.
    code += src[src.index('static const char *audio_device_label'):src.index('/* Hand the choice')]
    for name in ('load_audio_formats', 'load_audio_endpoints', 'select_audio_endpoint',
                 'sound_chip', 'sound_endpoint_row', 'paint_sound_content', 'sound_track', 'paint_sound'):
        code += function(src, name) + '\n'
    code += r'''
int main(void){
    settings_t st={0};st.have_audio=true;st.audio.output_ready=1;
    load_audio_endpoints(&st);assert(st.audio_ep_count==2&&st.audio_eps[0].current);
    select_audio_endpoint(&st,2,false);assert(strstr(st.status,"Input selected")&&st.audio_eps[1].current);
    select_audio_endpoint(&st,1,true);assert(strstr(st.status,"Output selected")&&rates_read==1);
    confirm=0;select_audio_endpoint(&st,1,true);assert(strstr(st.status,"did not confirm"));
    selection_error=1;int before=reads;select_audio_endpoint(&st,2,false);
    assert(strstr(st.status,"could not")&&reads==before);
    malformed=1;load_audio_endpoints(&st);assert(!st.audio_ep_count);
    assert(opened==closes);fail_open=1;select_audio_endpoint(&st,1,true);assert(strstr(st.status,"would not open"));
    unsigned cases=0;
    for(scale=1;scale<=3;scale++)for(int width=180;width<=2000;width+=113)
    for(int count=0;count<=24;count++)for(int direction=0;direction<3;direction++){
        surface_t s={width,500};st.audio_ep_count=count;
        for(int i=0;i<count;i++){st.audio_eps[i].is_output=direction==2?i%2:direction;
            st.audio_eps[i].node=i+1;st.audio_eps[i].device=2;}
        st.sound_scroll=0;paint_sound(&st,&s,-1,-1);
        assert(st.rc_out_n+st.rc_in_n==count&&st.sound_total>0);
        for(int dir=0;dir<2;dir++){
            rect_t *rs=dir?st.rc_out:st.rc_in;int n=dir?st.rc_out_n:st.rc_in_n;
            for(int i=1;i<n;i++)assert(rs[i-1].y+rs[i-1].h<rs[i].y);
        }
        int total=st.sound_total;st.sound_scroll=total;paint_sound(&st,&s,-1,-1);
        assert(st.sound_total==total&&st.sound_scroll==settings_scroll_clamp(total,total,420));
        s.height=5000;paint_sound(&st,&s,-1,-1);assert(st.sound_scroll==0);cases++;
    }
    printf("SETTINGS_SOUND_PASS %u layouts; all 24 endpoints reachable, selection readback and failures\n",cases);
}
'''
    run_test(code, 'settings_sound')


if __name__ == '__main__':
    main()
