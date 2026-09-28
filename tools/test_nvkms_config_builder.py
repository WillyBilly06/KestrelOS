#!/usr/bin/env python3
"""Execute the native request builder using NVIDIA's real 595.99.02 KAPI ABI.

No modeset/driver execution is simulated or claimed. This verifies immutable
request construction, retained timing bytes, detach state and rejection gates.
"""
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    src = (ROOT/'kernel/nvkms_kapi_client.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
'''
    code += '#include "'+(ROOT/'out/nvidia-open-595.99.02/kernel-open/common/inc/nvkms-kapi.h').as_posix()+'"\n'
    code += '#include "'+(ROOT/'include/kestrel/display_layout.h').as_posix()+'"\n'
    code += '#define KESTREL_NVKMS_MAX_TEST_DISPLAYS 4\n'
    a = src.index('typedef struct {\n    NvKmsKapiDisplay handle;')
    code += src[a:src.index('} test_display_t;', a)+len('} test_display_t;')]+'\n'
    code += r'''
static struct NvKmsKapiDeviceResourcesInfo resources;
static test_display_t active[4];
static struct NvKmsKapiRequestedModeSetConfig requested;
'''
    for name in ['set_all_layer_flags', 'build_display_config', 'build_requested_config']:
        code += function(src, name)+'\n'
    code += r'''
static void setup(test_display_t *d,dl_layout_t*l){
    memset(d,0,4*sizeof*d);memset(l,0,sizeof*l);memset(&resources,0,sizeof resources);
    resources.numHeads=4;l->n=4;
    for(int i=0;i<4;i++){
        resources.numLayers[i]=1;d[i].handle=1u<<i;d[i].head=i;d[i].static_info.headMask=15;
        d[i].mode.timings.hVisible=640+128*i;d[i].mode.timings.vVisible=480+32*i;
        d[i].mode.timings.refreshRate=59940+60000*i;d[i].mode.timings.pixelClockHz=148500000+i*197;
        d[i].mode.timings.hSyncStart=1000+i;d[i].mode.timings.hSyncEnd=1100+i;d[i].mode.timings.hTotal=1200+i;
        d[i].mode.timings.vSyncStart=800+i;d[i].mode.timings.vSyncEnd=810+i;d[i].mode.timings.vTotal=840+i;
        d[i].mode.timings.flags.hSyncPos=i&1;d[i].mode.timings.flags.vSyncNeg=!(i&1);
        snprintf(d[i].mode.name,sizeof d[i].mode.name,"exact-timing-%d",i);
        d[i].pitch=d[i].mode.timings.hVisible*4+64;
        d[i].bytes=(NvU64)d[i].pitch*d[i].mode.timings.vVisible;
        for(int s=0;s<2;s++){
            d[i].surface[s]=(void*)(uintptr_t)(0x10000+i*0x1000+s*0x100);
            d[i].memory[s]=(void*)(uintptr_t)(0x20000+i*0x1000+s*0x100);
        }
        l->out[i].active=1;l->out[i].out_w=d[i].mode.timings.hVisible;l->out[i].out_h=d[i].mode.timings.vVisible;
    }
}
static void rejected(test_display_t*d,NvU32 n,dl_layout_t*l,NvU32 previous){
    struct NvKmsKapiRequestedModeSetConfig out,before;
    memset(&out,0xa5,sizeof out);before=out;
    assert(!build_display_config(d,n,l,previous,&out));assert(!memcmp(&out,&before,sizeof out));
}
int main(void){
    test_display_t d[4],before[4];dl_layout_t l;
    struct NvKmsKapiRequestedModeSetConfig out,sentinel;
    memset(&requested,0x5a,sizeof requested);sentinel=requested;
    unsigned cases=0;
    for(unsigned a=0;a<4;a++)for(unsigned b=0;b<4;b++)if(b!=a)
    for(unsigned c=0;c<4;c++)if(c!=a&&c!=b)for(unsigned e=0;e<4;e++)if(e!=a&&e!=b&&e!=c)
    for(unsigned enabled=1;enabled<16;enabled++)for(unsigned previous=0;previous<16;previous++)for(unsigned front=0;front<2;front++){
        setup(d,&l);unsigned heads[4]={a,b,c,e},wanted=0;
        for(int i=0;i<4;i++){
            d[i].head=heads[i];d[i].front=front;l.out[i].active=!!(enabled&(1u<<i));
            l.out[i].rotation=(i+front)%4*90; /* compositor orientation, not NVKMS plane rotation */
            if(l.out[i].active)wanted|=1u<<heads[i];
        }
        memcpy(before,d,sizeof d);dl_layout_t oldl=l;
        assert(build_display_config(d,4,&l,previous,&out));cases++;
        assert(!memcmp(d,before,sizeof d)&&!memcmp(&l,&oldl,sizeof l)&&!memcmp(&requested,&sentinel,sizeof requested));
        assert(out.headsMask==(wanted|previous));
        for(unsigned head=0;head<4;head++){
            struct NvKmsKapiHeadRequestedConfig*h=&out.headRequestedConfig[head];
            struct NvKmsKapiLayerRequestedConfig*p=&h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
            if(!(wanted&(1u<<head))){
                assert(!h->modeSetConfig.bActive&&!h->modeSetConfig.numDisplays&&!p->config.surface);
                assert(!!h->flags.activeChanged==!!(previous&(1u<<head)));
                assert(!!h->flags.displaysChanged==!!(previous&(1u<<head)));
                assert(!!p->flags.surfaceChanged==!!(previous&(1u<<head)));continue;
            }
            unsigned i=0;while(heads[i]!=head)i++;
            assert(h->modeSetConfig.bActive&&h->modeSetConfig.numDisplays==1&&h->modeSetConfig.displays[0]==d[i].handle);
            assert(!memcmp(&h->modeSetConfig.mode,&d[i].mode,sizeof d[i].mode));
            assert(h->flags.activeChanged&&h->flags.displaysChanged&&h->flags.modeChanged);
            assert(h->flags.legacyIlutChanged&&h->flags.legacyOlutChanged);
            assert(h->modeSetConfig.olutFpNormScale==NVKMS_OLUT_FP_NORM_SCALE_DEFAULT);
            assert(h->modeSetConfig.lut.input.depth==30&&!h->modeSetConfig.lut.input.pRamps&&!h->modeSetConfig.lut.input.end);
            assert(!h->modeSetConfig.lut.output.enabled&&!h->modeSetConfig.lut.output.pRamps);
            assert(p->config.surface==d[i].surface[front]&&p->config.srcWidth==d[i].mode.timings.hVisible);
            assert(p->config.srcHeight==d[i].mode.timings.vVisible&&p->config.dstWidth==p->config.srcWidth&&p->config.dstHeight==p->config.srcHeight);
            assert(p->config.minPresentInterval==1&&!p->config.tearing&&p->config.rrParams.rotation==NVKMS_ROTATION_0);
            assert(p->config.compParams.surfaceAlpha==255&&p->config.compParams.compMode==NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE);
            assert(p->config.inputTf==NVKMS_INPUT_TF_LINEAR&&p->config.outputTf==NVKMS_OUTPUT_TF_NONE);
            assert(p->flags.surfaceChanged&&p->flags.srcXYChanged&&p->flags.srcWHChanged&&p->flags.dstXYChanged&&p->flags.dstWHChanged);
            assert(p->flags.cscChanged&&p->flags.inputTfChanged&&p->flags.outputTfChanged&&p->flags.inputColorSpaceChanged);
            assert(p->flags.inputColorRangeChanged&&p->flags.hdrMetadataChanged&&p->flags.matrixOverridesChanged&&p->flags.ilutChanged&&p->flags.tmoChanged);
        }
    }
    for(int fault=0;fault<14;fault++){
        setup(d,&l);
        if(fault==0)d[1].head=d[0].head;if(fault==1)d[1].handle=d[0].handle;
        if(fault==2)d[0].front=2;if(fault==3)d[0].surface[0]=NULL;if(fault==4)d[0].memory[0]=NULL;
        if(fault==5)d[0].pitch=1;if(fault==6)d[0].bytes--;
        if(fault==7)d[0].static_info.headMask=0;if(fault==8)resources.numLayers[0]=0;
        if(fault==9)d[0].head=32;if(fault==10)resources.numHeads=32;
        if(fault==11)l.out[0].active=2;if(fault==12)l.out[0].out_w++;if(fault==13)l.n=3;
        rejected(d,4,&l,15);
    }
    setup(d,&l);rejected(d,4,&l,16);rejected(d,5,&l,0);rejected(d,0,&l,0);rejected(NULL,4,&l,0);
    for(int i=0;i<4;i++)l.out[i].active=0;rejected(d,4,&l,15);
    setup(d,&l);memcpy(active,d,sizeof d);assert(build_requested_config(4));
    assert(build_display_config(d,4,NULL,0,&out)&&!memcmp(&requested,&out,sizeof out));
    /* Disabled outputs need no candidate surface and can relinquish a head
     * which is then assigned to another output in the same request. */
    setup(d,&l);l.out[1].active=0;d[1].surface[0]=NULL;d[1].memory[0]=NULL;
    d[0].head=1;assert(build_display_config(d,4,&l,15,&out));
    assert(!out.headRequestedConfig[0].modeSetConfig.bActive&&out.headRequestedConfig[1].modeSetConfig.displays[0]==d[0].handle);
    printf("PASS %u native config combinations using official NVIDIA ABI: exact timing/surface/flags, independent immutable requests, inactive-head detach, reassignment, rejection preserves destination\n",cases);
}
'''
    run_test(code, 'nvkms_config_builder', [ROOT/'out/nvidia-open-595.99.02/kernel-open/common/inc'])


if __name__ == '__main__':
    main()
