"""Actual Settings live Apply/Keep/Revert event handlers with modeled syscalls."""
from test_gpu_stable_candidate import ROOT,function,run_test
from test_nvkms_hotplug import source

def main():
    code=source()
    ui=(ROOT/'user/desktop/display_settings.h').read_text()
    extra=r'''
#define ENOENT 2
static int errno;
static uint64_t ui_now;
static uint64_t uptime_ms(void){return ui_now;}
static unsigned apply_calls,finish_calls;
static int64_t apply_token;
static int finish_result,finish_errno;
static bool finish_confirm;
static kdisplay_output_mode_request_t applied;
static int64_t fb_apply_output_mode(const kdisplay_output_mode_request_t *q){apply_calls++;applied=*q;return apply_token;}
static int fb_finish_output_mode(uint64_t token,bool confirm){assert(token==UINT64_C(0x100000001));finish_calls++;finish_confirm=confirm;errno=finish_errno;return finish_result;}
enum {WE_MOUSE_UP,WE_MOUSE_MOVE,WE_MOUSE_DOWN,WE_MOUSE_WHEEL};
typedef struct {int kind,x,y,wheel;}wevent_t;
static int gui_scale(void){return 1;}
static bool rect_contains(rect_t r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;}
static bool rect_empty(rect_t r){return r.w<=0||r.h<=0;}
static rect_t rect_make(int x,int y,int w,int h){return (rect_t){x,y,w,h};}
static int gui_scrollbar_hit(rect_t r,int off,int visible,int count,int y){(void)r;(void)visible;(void)count;(void)y;return off;}
'''
    for name in ('ds_can_apply','ds_finish_mode','ds_tick','ds_clamp_scroll','ds_event'):
        extra+=function(ui,name)+'\n'
    extra+=r'''
static display_settings_t make_settings(void){
    display_settings_t d={0};ds_load(&d);assert(d.output_count==3);
    d.inventory_pending=false;d.selected_output=1;d.outputs[1].flags=KDISPLAY_CONNECTED|KDISPLAY_ENABLED;
    d.mode_count=2;d.rate_count=2;d.rates[0]=0;d.rates[1]=1;d.mode_indices[0]=12;d.mode_indices[1]=97;
    d.selected_rate=0;d.visible_rows=2;d.row_height=20;
    d.viewport=(rect_t){0,0,400,400};d.rate_rect=(rect_t){200,100,180,80};
    d.apply_rect=(rect_t){0,300,100,30};d.keep_rect=(rect_t){110,300,100,30};d.revert_rect=(rect_t){220,300,100,30};
    return d;
}
static void check_settings_actions(void){
    // Recreate a stable, already-active boot inventory from the real watcher.
    path_count=active_count=3;paths[0]=0x200;paths[1]=0x800;paths[2]=0x2000;connected=0x2a00;
    watch_ready=false;watch_events=1;watch_dirty=0;nvkms_watch_boot_boundary();nvkms_watch_start();
    ui_now=0;apply_token=UINT64_C(0x100000001);finish_result=finish_errno=0;
    display_settings_t d=make_settings();
    wevent_t ev={WE_MOUSE_DOWN,210,125,0};assert(ds_event(&d,&ev)&&d.selected_rate==1);
    ev=(wevent_t){WE_MOUSE_DOWN,10,310,0};assert(ds_event(&d,&ev)&&apply_calls==1);
    assert(applied.output_id==0x800&&applied.connector_id==0x800&&applied.generation==1&&applied.mode_index==97);
    assert(d.mode_token==UINT64_C(0x100000001)&&d.mode_deadline==20000&&!ds_can_apply(&d));
    ev=(wevent_t){WE_MOUSE_DOWN,120,310,0};assert(ds_event(&d,&ev)&&finish_calls==1&&finish_confirm&&!d.mode_token);
    d=make_settings();d.mode_token=UINT64_C(0x100000001);d.mode_deadline=20000;
    ui_now=19999;ds_tick(&d);assert(d.mode_token&&finish_calls==1);
    ui_now=20000;ds_tick(&d);assert(!d.mode_token&&finish_calls==2&&!finish_confirm);
    d=make_settings();d.mode_token=UINT64_C(0x100000001);
    finish_result=-1;finish_errno=5;ev=(wevent_t){WE_MOUSE_DOWN,230,310,0};
    assert(ds_event(&d,&ev)&&d.mode_token&&strstr(d.mode_status,"failed (5)"));
    finish_errno=ENOENT;assert(ds_event(&d,&ev)&&!d.mode_token); // watchdog already restored
    for(unsigned blocked=0;blocked<6;blocked++){
        d=make_settings();if(blocked==0)d.inventory_pending=true;
        if(blocked==1)d.outputs[1].flags|=KDISPLAY_STALE;
        if(blocked==2)d.outputs[1].flags|=KDISPLAY_DETECTED_ONLY;
        if(blocked==3)d.outputs[1].flags&=~KDISPLAY_ENABLED;
        if(blocked==4)d.outputs[1].flags&=~KDISPLAY_CONNECTED;
        if(blocked==5)d.selected_rate=-1;
        assert(!ds_can_apply(&d));
    }
    d=make_settings();apply_token=-1;errno=22;ev=(wevent_t){WE_MOUSE_DOWN,10,310,0};
    assert(ds_event(&d,&ev)&&!d.mode_token&&strstr(d.mode_status,"rejected (22)"));
    puts("PASS Settings live timing events: sorted native mode index, real Apply syscall, full-width token, Keep/Revert, countdown, errors and stale/inactive-output gating");
}
'''
    code=code.replace('static void changed(NvU32 h){',extra+'\nstatic void changed(NvU32 h){')
    code=code.replace('    puts("PASS deferred hotplug:', '    check_settings_actions();\n    puts("PASS deferred hotplug:')
    run_test(code,'output-mode-settings',[ROOT/'out/nvidia-open-595.99.02/kernel-open/common/inc'])

if __name__=='__main__':main()
