#!/usr/bin/env python3
"""Execute production display cache, syscall snapshot and Settings mode model.

The KAPI mode pool and committed heads are modeled, not physical monitors.
"""
import re
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    client = (ROOT / 'kernel/nvkms_kapi_client.c').read_text()
    ui = (ROOT / 'user/desktop/display_settings.h').read_text()
    abi = (ROOT / 'include/kestrel/display_info.h').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
typedef uint32_t NvU32,u32; typedef uint64_t NvU64,u64; typedef int64_t s64;
typedef bool NvBool; typedef unsigned NvKmsKapiDisplay;
#define NV_TRUE true
#define NV_FALSE false
#define KESTREL_NVKMS_MAX_TEST_DISPLAYS 4
#define E_INVAL 22
#define E_NOENT 2
struct NvKmsKapiDisplayMode { struct { unsigned hVisible,vVisible,refreshRate;
    struct {bool interlaced;} flags; unsigned clock,sync_start,sync_end,total; } timings; char name[32]; };
typedef struct { unsigned handle,connector_handle;bool connector_is_dp;
    char manufacturer[4],model[16];struct NvKmsKapiDisplayMode mode;} test_display_t;
typedef struct {int active,src_x,src_y,src_w,src_h,rotation;} dl_scanout_t;
static test_display_t active[4];
static unsigned active_count,runtime_primary_index,runtime_mode;
static bool runtime_ready;
static struct {dl_scanout_t out[4];} runtime_layout;
static struct NvKmsKapiDisplayMode pool[16];
static bool valid[16],accepted[16],preferred[16];
static int pool_count,queries,validations,fail_at=-1,test_device;
static size_t strlcpy(char *d,const char *s,size_t n) {size_t len=strlen(s);if(n){size_t c=len<n-1?len:n-1;memcpy(d,s,c);d[c]=0;}return len;}
static int get_mode(int dev,unsigned display,unsigned i,struct NvKmsKapiDisplayMode *out,bool *v,bool *p){
    assert(!dev&&display==active[0].handle);queries++;
    if((int)i==fail_at||i>=(unsigned)pool_count)return -1;
    *out=pool[i];*v=valid[i];*p=preferred[i];return i+1<(unsigned)pool_count;
}
static bool validate(int dev,unsigned display,const struct NvKmsKapiDisplayMode *m){
    assert(!dev&&display==active[0].handle);validations++;
    for(int i=0;i<pool_count;i++)if(!memcmp(m,&pool[i],sizeof *m))return accepted[i];
    assert(false);return false;
}
static struct {int (*getDisplayMode)(int,unsigned,unsigned,struct NvKmsKapiDisplayMode*,bool*,bool*);
    bool (*validateDisplayMode)(int,unsigned,const struct NvKmsKapiDisplayMode*);} kapi={get_mode,validate};
'''
    code += abi
    code += '''
static int nvkms_watch_output(u32 i,kdisplay_output_t *o){(void)i;(void)o;return -1;}
static int nvkms_watch_mode(u32 i,u32 m,kdisplay_mode_t *o){(void)i;(void)m;(void)o;return -1;}
'''
    start = client.index('static struct {\n    NvU32 handle, count;')
    code += client[start:client.index(';', client.index('} display_modes[', start)) + 1]
    for name in ['cache_display_mode', 'resolve_display_mode', 'select_max_mode', 'nvkms_kapi_display_output', 'nvkms_kapi_display_mode']:
        code += '\n' + function(client, name)
    code += r'''
static bool allowed=true;static size_t checked;
static bool user_range_ok(u64 p,size_t n,bool write){assert(write);checked=n;return allowed&&p;}
'''
    syscall = (ROOT / 'kernel/syscall.c').read_text()
    for name in ['enum_display_output', 'enum_display_output_mode']:
        # Actual syscall handlers are kept distinct from libc's names below.
        code += '\n' + function(syscall.replace('static s64 ', 'static long '), name).replace(name+'(', 'sys_'+name+'(', 1)
    code += r'''
static int enum_display_output(uint32_t i,kdisplay_output_t *out){return (int)sys_enum_display_output(i,(uintptr_t)out);}
static int enum_display_output_mode(uint32_t i,uint32_t mode,kdisplay_mode_t *out){
    assert(i<KDISPLAY_MAX_OUTPUTS&&mode<KDISPLAY_MAX_MODES);
    return (int)sys_enum_display_output_mode((i<<16)|mode,(uintptr_t)out);
}
typedef struct {int x,y,w,h;} rect_t;
'''
    start = ui.index('typedef struct {')
    code += ui[start:ui.index('} display_settings_t;', start) + len('} display_settings_t;')]
    for name in ['ds_format_hz', 'ds_rates', 'ds_select_output', 'ds_load', 'ds_clamp_scroll', 'ds_boot_slot']:
        code += '\n' + function(ui, name)
    code += r'''
int main(void){
    _Static_assert(sizeof(kdisplay_mode_t)==16,"mode ABI");
    _Static_assert(sizeof(kdisplay_output_t)==112,"output ABI");
    active[0]=(test_display_t){.handle=0x200,.connector_handle=17,.connector_is_dp=true};
    strcpy(active[0].manufacturer,"ACR");strcpy(active[0].model,"Test panel");
    pool_count=6;
    pool[0]=(struct NvKmsKapiDisplayMode){.timings={1920,1080,59940,{false}}};
    pool[1]=(struct NvKmsKapiDisplayMode){.timings={3840,2160,144000,{false}}};
    pool[2]=(struct NvKmsKapiDisplayMode){.timings={1920,1080,60000,{false}}};
    pool[3]=(struct NvKmsKapiDisplayMode){.timings={7680,4320,240000,{false}}};
    pool[4]=(struct NvKmsKapiDisplayMode){.timings={3840,2160,240000,{false}}};
    pool[5]=(struct NvKmsKapiDisplayMode){.timings={1920,1080,60000,{true}}};
    for(int i=0;i<pool_count;i++){
        pool[i].timings.clock=148500000+i*173;
        pool[i].timings.sync_start=2000+i;pool[i].timings.sync_end=2050+i;pool[i].timings.total=2200+i;
        snprintf(pool[i].name,sizeof pool[i].name,"exact-native-timing-%d",i);
    }
    for(int i=0;i<pool_count;i++)valid[i]=accepted[i]=true;
    accepted[3]=false;valid[4]=false;preferred[1]=true;
    struct NvKmsKapiDisplayMode best={0};
    assert(select_max_mode(0x200,&best)&&best.timings.hVisible==3840&&best.timings.refreshRate==144000);
    assert(queries==6&&validations==5&&display_modes[0].count==4);
    cache_display_mode(0,&pool[0],true);assert(display_modes[0].count==4);
    active[0].mode=pool[0];active_count=1;
    struct NvKmsKapiDisplayMode resolved;
    assert(resolve_display_mode(0,0x200,17,1,&resolved)&&!memcmp(&resolved,&pool[1],sizeof resolved));
    memset(&resolved,0xa5,sizeof resolved);struct NvKmsKapiDisplayMode unchanged=resolved;
    assert(!resolve_display_mode(0,0x200,18,1,&resolved)&&!memcmp(&resolved,&unchanged,sizeof resolved));
    assert(!resolve_display_mode(0,0x201,17,1,&resolved));
    assert(!resolve_display_mode(0,0x200,17,4,&resolved));
    assert(!resolve_display_mode(4,0x200,17,0,&resolved));
    assert(!resolve_display_mode(0,0x200,17,0,NULL));
    // A repeated label must not overwrite its retained validated timing with
    // another porch/clock variant just because the dimensions/rate match.
    struct NvKmsKapiDisplayMode alternate=pool[0];alternate.timings.clock++;
    cache_display_mode(0,&alternate,true);
    assert(resolve_display_mode(0,0x200,17,0,&resolved)&&!memcmp(&resolved,&pool[0],sizeof resolved));
    runtime_primary_index=0;runtime_mode=1;
    runtime_layout.out[0]=(dl_scanout_t){1,3840,120,1920,1080};
    kdisplay_output_t out;memset(&out,0xa5,sizeof out);
    assert(!nvkms_kapi_display_output(0,&out)&&out.version==0xa5a5a5a5);
    runtime_ready=true;
    assert(enum_display_output(0,&out)==0&&checked==sizeof out);
    assert(out.width==1920&&out.height==1080&&out.refresh_millihz==59940);
    assert(out.mode_count==4&&out.x==3840&&out.y==120&&out.group_mode==1);
    assert(out.flags==(KDISPLAY_CONNECTED|KDISPLAY_ENABLED|KDISPLAY_PRIMARY));
    assert(!strcmp(out.manufacturer,"ACR")&&!strcmp(out.connector,"DisplayPort")&&!out.reserved);
    assert(!nvkms_kapi_display_output(1,&out)&&!nvkms_kapi_display_output(UINT_MAX,&out));
    kdisplay_mode_t mode;
    assert(enum_display_output_mode(0,0,&mode)==0&&checked==sizeof mode);
    assert(mode.refresh_millihz==59940&&(mode.flags&KDISPLAY_MODE_CURRENT));
    assert(mode.flags&KDISPLAY_MODE_PREFERRED);
    assert(enum_display_output_mode(0,3,&mode)==0&&(mode.flags&KDISPLAY_MODE_INTERLACED));
    assert(!(mode.flags&KDISPLAY_MODE_CURRENT));
    assert(enum_display_output_mode(0,4,&mode)==-E_NOENT);
    allowed=false;memset(&mode,0xa5,sizeof mode);
    assert(enum_display_output_mode(0,0,&mode)==-E_INVAL&&mode.width==0xa5a5a5a5);allowed=true;
    display_settings_t d={0};ds_load(&d);
    assert(d.output_count==1&&d.resolution_count==2&&d.mode_count==4);
    assert(d.modes[d.resolution[0]].width==3840&&d.selected_resolution==1);
    assert(d.rate_count==3&&d.modes[d.rates[d.selected_rate]].refresh_millihz==59940);
    d.selected_resolution=0;ds_rates(&d);
    assert(d.rate_count==1&&d.modes[d.rates[0]].refresh_millihz==144000&&d.selected_rate==-1);
    char hz[24];ds_format_hz(59940,hz,sizeof hz);assert(!strcmp(hz,"59.940 Hz"));
    ds_format_hz(239999,hz,sizeof hz);assert(!strcmp(hz,"239.999 Hz"));
    int n=INT_MAX;ds_clamp_scroll(&n,4,3);assert(n==1);
    n=-8;ds_clamp_scroll(&n,4,3);assert(!n);
    // Replaced connector identity cannot leak a previous mode cache.
    display_modes[0].handle=99;
    assert(nvkms_kapi_display_output(0,&out)&&!out.mode_count);
    assert(!nvkms_kapi_display_mode(0,0,&mode));display_modes[0].handle=active[0].handle;
    display_modes[0].count=KDISPLAY_MAX_MODES;
    cache_display_mode(0,&pool[3],false);assert(display_modes[0].truncated);
    assert(nvkms_kapi_display_output(0,&out)&&(out.flags&KDISPLAY_MODES_TRUNCATED));
    assert(queries==6&&validations==5); // UI/syscalls never call into KAPI.
    d.output_count=4;
    for(int primary=0;primary<4;primary++){
        for(int i=0;i<4;i++)d.outputs[i].flags=i==primary?KDISPLAY_PRIMARY:0;
        int boot[4],next=1;boot[0]=primary;
        for(int i=0;i<4;i++)if(i!=primary)boot[next++]=i;
        for(int i=0;i<4;i++)assert(boot[ds_boot_slot(&d,i)-1]==i);
    }
    puts("PASS actual display cache/syscalls/Settings model: committed timing, validated paired modes, exact fractional Hz, interlace/preference, primary/position, bounds, truncation, stale identity, no UI-side probing (modeled KAPI)");
}
'''
    run_test(code, 'display_inventory')


if __name__ == '__main__':
    main()
