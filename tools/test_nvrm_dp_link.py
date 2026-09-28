#!/usr/bin/env python3
"""Host checks of production DP readback; no link training/GPU emulation."""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    os_source = (ROOT / 'kernel/nvrm_os.c').read_text()
    rm_source = (ROOT / 'kernel/nv_gsp_rm.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvtypes.h"
#include "nvstatus.h"
#include "ctrl/ctrl0073/ctrl0073dp.h"
typedef uint32_t u32; typedef uint8_t u8;
#include "nvrm_dp_link.h"
static unsigned calls,scenario;
static u32 nvrm_host_control_object(u32 c,u32 o,u32 cmd,void *p,u32 size){
    assert(c==0x1234&&o==0x730000);unsigned step=calls++;
    assert(step<4);
    if(step==2){
        assert(cmd==NV0073_CTRL_CMD_DP_GET_MSA_ATTRIBUTES&&size==sizeof(NV0073_CTRL_DP_GET_MSA_ATTRIBUTES_PARAMS));
        NV0073_CTRL_DP_GET_MSA_ATTRIBUTES_PARAMS *v=p;
        NV0073_CTRL_DP_GET_MSA_ATTRIBUTES_PARAMS empty={.displayId=0x800};
        assert(!memcmp(v,&empty,sizeof *v));
        if(scenario==18)return NV_ERR_NOT_SUPPORTED;
        v->mvid=123;v->nvid=32768;v->hTotal=2700;v->vTotal=1500;
        v->hActiveStart=100;v->vActiveStart=40;v->hActiveWidth=2560;v->vActiveWidth=1440;
        v->hSyncWidth=32;v->vSyncWidth=8;v->hSyncPolarity=true;v->misc0=0x20;v->misc1=0x80;
        return NV_OK;
    }
    if(step!=1){
        assert(cmd==NV0073_CTRL_CMD_DP_GET_LINK_CONFIG);
        assert(size==sizeof(NV0073_CTRL_DP_GET_LINK_CONFIG_PARAMS));
        NV0073_CTRL_DP_GET_LINK_CONFIG_PARAMS *v=p;
        NV0073_CTRL_DP_GET_LINK_CONFIG_PARAMS empty={.displayId=0x800};
        assert(!memcmp(v,&empty,sizeof *v));
        if((scenario==1&&step==0)||(scenario==2&&step==3))return NV_ERR_GENERIC;
        v->laneCount=4;v->linkBW=0x1e;v->bFECEnabled=true;
        if(scenario==3&&step==3)v->linkBW=0x14;
        if(scenario==4){v->linkBW=0;v->dp2LinkBW=1000;}
        if(scenario==5)v->laneCount=0;
        if(scenario==6)v->laneCount=3;
        if(scenario==7)v->laneCount=1;
        if(scenario==8)v->laneCount=2;
        return NV_OK;
    }
    assert(cmd==NV0073_CTRL_CMD_DP_AUXCH_CTRL&&size==sizeof(NV0073_CTRL_DP_AUXCH_CTRL_PARAMS));
    NV0073_CTRL_DP_AUXCH_CTRL_PARAMS *v=p;
    NV0073_CTRL_DP_AUXCH_CTRL_PARAMS empty={.displayId=0x800,.cmd=9,.addr=0x200,.size=5,.replyType=0xffffffff};
    assert(!memcmp(v,&empty,sizeof *v)); // read-only, instance zero, exact count-minus-one
    v->replyType=0;v->size=6;
    v->data[0]=1;v->data[2]=v->data[3]=0x77;v->data[4]=v->data[5]=1;
    if(scenario==7){v->data[2]=7;v->data[3]=0;}
    if(scenario==8)v->data[3]=0;
    if(scenario==9)v->data[3]=0x67; // last lane loses clock recovery
    if(scenario==10)v->data[4]=0;
    if(scenario==11)return NV_ERR_GENERIC;
    if(scenario==12)v->replyType=NV0073_CTRL_DP_AUXCH_REPLYTYPE_DEFER;
    if(scenario==13)v->replyType=NV0073_CTRL_DP_AUXCH_REPLYTYPE_NACK;
    if(scenario==14)v->size=5; // must NOT accept legacy helper's count-minus-one guess
    if(scenario==15)v->size=0;
    if(scenario==16)v->size=7;
    if(scenario==17)v->retryTimeMs=1;
    return NV_OK;
}
''' + function(os_source, 'nvrm_host_read_dp_link') + r'''
typedef struct {bool ready,up,host_api,display_query_attempted;u32 display_query_status;} nv_rm_t;
typedef int nv_card_t;
#define NV_CTRL_FAIL_NOT_READY 0xffffffffu
#define RM_DEVICE 0x800000u
#define RM_DISP 0x730000u
#define NV04_DISPLAY_COMMON 0x73u
static u32 nv_last_alloc_status;
static unsigned allocations;
static bool nv_rm_alloc(nv_card_t *c,nv_rm_t *rm,u32 parent,u32 object,u32 klass,void *p,u32 n){
    assert(c&&rm&&parent==RM_DEVICE&&object==RM_DISP&&klass==NV04_DISPLAY_COMMON&&!p&&!n);
    allocations++;return nv_last_alloc_status==NV_OK;
}
''' + function(rm_source, 'nv_rm_host_display_query_object') + r'''
int main(void){
    nvrm_dp_link_t link;
    nvrm_host_read_dp_link(0x1234,0x730000,0x800,NULL);assert(!calls);
    nvrm_host_read_dp_link(0,0x730000,0x800,&link);assert(!calls&&!link.tx_valid&&!link.rx_valid);
    nvrm_host_read_dp_link(0x1234,0,0x800,&link);assert(!calls);
    nvrm_host_read_dp_link(0x1234,0x730000,0,&link);assert(!calls);
    nvrm_host_read_dp_link(0x1234,0x730000,0xa00,&link);assert(!calls);
    for(scenario=0;scenario<=18;scenario++){
        memset(&link,0xff,sizeof link);calls=0;
        nvrm_host_read_dp_link(0x1234,0x730000,0x800,&link);
        assert(calls==4);
        if(scenario==0||scenario==7||scenario==8||scenario==18){
            assert(link.tx_valid&&link.rx_valid&&link.tx_config_stable&&link.legacy_lock_known&&link.legacy_locked);
        }else if(scenario==9||scenario==10){
            assert(link.legacy_lock_known&&!link.legacy_locked);
        }else assert(!link.legacy_lock_known&&!link.legacy_locked);
        if(scenario>=11&&scenario<=17){assert(!link.rx_valid);for(unsigned j=0;j<6;j++)assert(!link.receiver[j]);}
        if(scenario==18)assert(link.msa_status==NV_ERR_NOT_SUPPORTED&&!link.width&&!link.mvid);
        else assert(link.msa_status==NV_OK&&link.width==2560&&link.height==1440&&link.h_total==2700&&link.v_total==1500&&
            link.h_start==100&&link.v_start==40&&link.h_sync==32&&link.v_sync==8&&link.h_positive&&!link.v_positive&&
            link.mvid==123&&link.nvid==32768&&link.misc0==0x20&&link.misc1==0x80);
        if(scenario==4)assert(link.rx_valid&&link.rate_10mbps==1000&&!link.rate_code);
        if(scenario==3)assert(link.tx_valid&&link.rx_valid&&!link.tx_config_stable);
    }
    nv_card_t card=0;u32 object=99;
    nv_rm_t rm={0};
    assert(nv_rm_host_display_query_object(&card,&rm,&object)!=NV_OK&&!object&&!allocations);
    rm.ready=rm.up=true;
    assert(nv_rm_host_display_query_object(&card,&rm,&object)!=NV_OK&&!object&&!allocations);
    rm.host_api=true;
    assert(nv_rm_host_display_query_object(&card,&rm,NULL)!=NV_OK&&!allocations);
    nv_last_alloc_status=NV_ERR_NO_MEMORY;
    assert(nv_rm_host_display_query_object(&card,&rm,&object)==NV_ERR_NO_MEMORY&&!object&&allocations==1);
    assert(nv_rm_host_display_query_object(&card,&rm,&object)==NV_ERR_NO_MEMORY&&!object&&allocations==1);
    rm=(nv_rm_t){.ready=true,.up=true,.host_api=true};nv_last_alloc_status=NV_OK;
    assert(nv_rm_host_display_query_object(&card,&rm,&object)==NV_OK&&object==RM_DISP&&allocations==2);
    assert(nv_rm_host_display_query_object(&card,&rm,&object)==NV_OK&&object==RM_DISP&&allocations==2);
    puts("PASS production DP snapshots: exact official read ABI, stable TX, strict AUX ACK/count, legacy lane failures, UHBR unknown, invalid inputs and one-time query-object gates");
}
'''
    run_test(code, 'nvrm_dp_link', [ROOT / 'kernel',
        ROOT / 'out/nvidia-open-595.99.02/src/common/sdk/nvidia/inc'])


if __name__ == '__main__':
    main()
