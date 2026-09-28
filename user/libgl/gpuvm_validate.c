/* A native readback gate for vertex and fragment bytecode execution. Small
 * dyadic fixtures compare exactly; the CPU supplies inputs and checks outputs,
 * but never executes the program as a substitute for the GPU dispatch. */
#include "gpuvm.h"

static sh_instruction_t vm_ins(unsigned op,unsigned dst,unsigned a,unsigned b,unsigned c) {
    sh_instruction_t in={.op=op,.mask=15,.dst=dst,.src={a,b,c},.swizzle={228,228,228}};
    return in;
}

bool gl_gpu_vm_validate(void) {
    enum { LANES=67, FLOATS=LANES*SR_REGISTERS*4 };
    gl_gpu_vm_t vm={0};
    ksh_dispatch_t *job=calloc(1,sizeof(*job));
    float *registers=calloc(FLOATS,sizeof(float));
    float *out=malloc(LANES*4*sizeof(float));
    const char *stage="CPU input allocation";
    bool ok=false;
    if(!job||!registers||!out){vm.last_error=-ENOMEM;goto done;}
    job->lanes=LANES;job->budget=8;job->code_count=3;
    #define REG(r,k,l) registers[((r)*4+(k))*LANES+(l)]
    for(unsigned lane=0;lane<LANES;lane++){
        REG(SR_ATTRIB,0,lane)=(float)lane/16;REG(SR_ATTRIB,1,lane)=-0.5f;
        REG(SR_ATTRIB,2,lane)=0.25f;REG(SR_ATTRIB,3,lane)=1;
        REG(SR_UNIFORM,0,lane)=2;REG(SR_UNIFORM+1,1,lane)=3;
        REG(SR_UNIFORM+2,2,lane)=4;
        REG(SR_UNIFORM+3,0,lane)=0.5f;REG(SR_UNIFORM+3,1,lane)=-0.25f;
        REG(SR_UNIFORM+3,2,lane)=1;REG(SR_UNIFORM+3,3,lane)=1;
        for(unsigned k=0;k<4;k++){
            REG(SR_CONST,k,lane)=0.5f;REG(SR_UNIFORM+4,k,lane)=0.25f;
        }
        REG(SR_VARYING,3,lane)=1;
    }
    job->code[0]=vm_ins(SH_MATMUL,SR_POSITION,SR_UNIFORM,SR_ATTRIB,0);
    job->code[1]=vm_ins(SH_MAD,SR_VARYING,SR_ATTRIB,SR_CONST,SR_UNIFORM+4);
    job->code[1].mask=7;
    job->code[2]=vm_ins(SH_END,0,0,0,0);
    stage="vertex dispatch/status";
    if(!gl_gpu_vm_execute(&vm,job,registers,FLOATS,NULL,0))goto done;
    stage="vertex position readback";
    if(!gl_gpu_vm_read(&vm,SR_POSITION,1,out,LANES*4))goto done;
    for(unsigned lane=0;lane<LANES;lane++){
        vm.bad_lane=lane;
        if(vm.results[lane].result!=KSH_COMPLETE||vm.results[lane].executed!=3||
           out[lane]!=(float)lane/8+0.5f||out[LANES+lane]!=-1.75f||
           out[LANES*2+lane]!=2||out[LANES*3+lane]!=1)goto done;
    }
    stage="vertex varying/mask readback";
    if(!gl_gpu_vm_read(&vm,SR_VARYING,1,out,LANES*4))goto done;
    for(unsigned lane=0;lane<LANES;lane++){
        vm.bad_lane=lane;
        if(out[lane]!=(float)lane/32+0.25f||out[LANES+lane]!=0||
           out[LANES*2+lane]!=0.375f||out[LANES*3+lane]!=1)goto done;
    }

    memset(registers,0,FLOATS*sizeof(float));
    job->code_count=4;
    for(unsigned lane=0;lane<LANES;lane++){
        REG(SR_ATTRIB,0,lane)=(float)(lane&1);
        REG(SR_VARYING,0,lane)=(float)lane/128;REG(SR_VARYING,1,lane)=0.25f;
        REG(SR_VARYING,2,lane)=0.5f;REG(SR_VARYING,3,lane)=1;
        REG(SR_UNIFORM,0,lane)=0.5f;REG(SR_UNIFORM,1,lane)=1;
        REG(SR_UNIFORM,2,lane)=0.25f;REG(SR_UNIFORM,3,lane)=1;
        REG(SR_FRAGCOLOR,3,lane)=1;
    }
    job->code[0]=vm_ins(SH_JMPZ,0,SR_ATTRIB,0,0);job->code[0].target=2;
    job->code[1]=vm_ins(SH_DISCARD,0,0,0,0);
    job->code[2]=vm_ins(SH_MUL,SR_FRAGCOLOR,SR_VARYING,SR_UNIFORM,0);
    job->code[3]=vm_ins(SH_END,0,0,0,0);
    stage="fragment discard dispatch/status";
    if(!gl_gpu_vm_execute(&vm,job,registers,FLOATS,NULL,0))goto done;
    stage="fragment colour/discard readback";
    if(!gl_gpu_vm_read(&vm,SR_FRAGCOLOR,1,out,LANES*4))goto done;
    for(unsigned lane=0;lane<LANES;lane++){
        vm.bad_lane=lane;
        if(lane&1){
            if(vm.results[lane].result!=KSH_DISCARDED||vm.results[lane].executed!=2)goto done;
        }else if(vm.results[lane].result!=KSH_COMPLETE||vm.results[lane].executed!=3||
                 out[lane]!=(float)lane/256||out[LANES+lane]!=0.25f||
                 out[LANES*2+lane]!=0.125f||out[LANES*3+lane]!=1)goto done;
    }

    memset(registers,0,FLOATS*sizeof(float));
    for(unsigned lane=0;lane<LANES;lane++)REG(SR_ATTRIB,0,lane)=REG(SR_ATTRIB,1,lane)=0.25f;
    job->code_count=2;job->texture_count=1;
    job->textures[0]=(ksh_texture_t){.width=2,.height=2,.pitch=2,.wrap_s=1,.wrap_t=1};
    job->code[0]=vm_ins(SH_TEX,SR_FRAGCOLOR,SR_ATTRIB,SR_UNIFORM,0);
    job->code[1]=vm_ins(SH_END,0,0,0,0);
    static const unsigned pixels[4]={0xffff0000u,0xff00ff00u,0xff0000ffu,0xffffffffu};
    stage="texture dispatch/status";
    if(!gl_gpu_vm_execute(&vm,job,registers,FLOATS,pixels,sizeof pixels))goto done;
    stage="texture blue-texel readback";
    if(!gl_gpu_vm_read(&vm,SR_FRAGCOLOR,1,out,LANES*4))goto done;
    for(unsigned lane=0;lane<LANES;lane++){
        vm.bad_lane=lane;
        if(vm.results[lane].result!=KSH_COMPLETE||vm.results[lane].executed!=2||
           out[lane]!=0||out[LANES+lane]!=0||out[LANES*2+lane]!=1||out[LANES*3+lane]!=1)goto done;
    }
    #undef REG
    ok=true;
done:
    if(!ok){
        char note[224];snprintf(note,sizeof note,"native shader gate FAILED at %s: transport=%lld lane=%u result=%u quarantined=%u",
            stage,(long long)vm.last_error,vm.bad_lane,vm.lane_error,vm.quarantined);
        log_write(3,"gl-vm",note);
    }
    if(!gl_gpu_vm_release(&vm)){
        log_write(3,"gl-vm","native shader gate cleanup failed or buffers quarantined; not a PASS");
        ok=false;
    }
    free(out);free(registers);free(job);
    if(ok)log_write(1,"gl-vm","native shader VM gate PASS: 67 lanes, GPU matrix/varying-mask, fragment discard/colour and texture readbacks; no CPU shader fallback");
    return ok;
}
