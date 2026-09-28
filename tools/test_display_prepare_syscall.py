"""Production prepare/cancel libc/dispatcher/ownership and user-memory boundary."""
import re
from test_gpu_stable_candidate import ROOT,function,run_test
from test_display_configuration_syscall import source

def main():
    code=source()
    abi=(ROOT/'include/kestrel/syscall.h').read_text()
    for name,value in [('FB_PREPARE_CONFIGURATION',13),('FB_CANCEL_CONFIGURATION',14)]:
        assert int(re.search(r'^#define '+name+r'\s+(\d+)',abi,re.M)[1])==value
        code=f'#define {name} {value}\n'+code
    decl=r'''
typedef struct {u32 pid;u64 gpu_owner_id;bool resource_closing;}proc_t;
static proc_t process={7,UINT64_C(0x177775555),false};
static proc_t *proc_shared(proc_t *p){return p;}
static bool prepare_mode;
static kdisplay_prepared_configuration_t prepare_answer;
static const u64 expected_token=UINT64_C(0x123456789abcdef);
static int cancel_calls;
static bool irq_disabled,revoke_result;
'''
    code=code.replace('static void *out_address;',decl+'\nstatic void *out_address;')
    code=code.replace('size==sizeof answer&&!bad_write',
                      'size==(prepare_mode?sizeof prepare_answer:sizeof answer)&&!bad_write')
    extra=r'''
/* Production prepare now uses the address-space-locked copy helper introduced
 * with application SMP. Model its checked copy boundary, not the old IRQ-only
 * memcpy sequence; hardware behavior is still outside this syscall test. */
static bool user_copy(void *buffer,u64 address,size_t bytes,bool to_user){
    if(!user_range_ok(address,bytes,to_user))return false;
    if(to_user)memcpy((void*)(uintptr_t)address,buffer,bytes);
    else memcpy(buffer,(const void*)(uintptr_t)address,bytes);
    return true;
}
static int nvkms_kapi_prepare_configuration(u64 owner,const kdisplay_configuration_t *q,kdisplay_prepared_configuration_t *out){
    assert(held&&!irq_disabled&&owner==process.gpu_owner_id&&owner>UINT32_MAX&&q!=&request&&!memcmp(q,&expected,sizeof *q));calls++;
    if(verdict)return verdict;
    memset(out,0,sizeof *out);out->configuration.version=1;out->configuration.generation=q->generation;
    out->token=expected_token;out->expires_us=30000000;if(revoke_result)bad_write=true;return 0;
}
static int nvkms_kapi_cancel_configuration(u64 owner,u64 token){
    assert(held&&owner==process.gpu_owner_id&&token==expected_token);cancel_calls++;return verdict;
}
'''
    src=(ROOT/'kernel/syscall.c').read_text()
    extra+=function(src.replace('static s64 ','static long '),'sys_fb_prepare_configuration')+'\n'
    code=code.replace('static long dispatch(',extra+'static long dispatch(')
    start=src.index('        if (a1 == FB_PREPARE_CONFIGURATION',src.index('case SYS_FRAMEBUFFER:'))
    end=src.index('\n        }',start)+10
    branch=src[start:end].replace('sys_fb_prepare_configuration(p,','sys_fb_prepare_configuration(&process,')
    code=code.replace('return -E_NOSYS;\n}\n',branch+'\nreturn -E_NOSYS;\n}\n',1)
    libc=(ROOT/'user/libc/syscalls.c').read_text()
    wrappers=function(libc,'fb_prepare_configuration')+'\n'+function(libc,'fb_cancel_configuration')+'\n'
    code=code.replace('static void reset(void)',wrappers+'static void reset(void)')
    checks=r'''
static void prep_reset(void){
    assert(!irq_disabled);reset();revoke_result=false;prepare_mode=true;process=(proc_t){7,UINT64_C(0x177775555),false};cancel_calls=0;
    memset(&prepare_answer,0xa5,sizeof prepare_answer);out_address=&prepare_answer;
}
static void check_prepare_syscall(void){
    prep_reset();assert(!fb_prepare_configuration(&request,&prepare_answer));
    assert(calls==1&&begins==1&&ends==1&&prepare_answer.token==expected_token);
    prep_reset();mutate=true;assert(!fb_prepare_configuration(&request,&prepare_answer)&&prepare_answer.configuration.generation==32);
    prep_reset();out_address=&request;mutate=true;
    assert(!fb_prepare_configuration(&request,(void*)&request)&&request.generation==32);
    prep_reset();revoke_result=true;kdisplay_prepared_configuration_t sentinel=prepare_answer;
    assert(fb_prepare_configuration(&request,&prepare_answer)==-1&&errno==E_INVAL&&cancel_calls==1);
    assert(!memcmp(&prepare_answer,&sentinel,sizeof sentinel)&&!irq_disabled&&begins==ends);
    for(unsigned i=0;i<3;i++){
        prep_reset();if(i==0)process.pid=8;if(i==1)process.gpu_owner_id=0;if(i==2)process.resource_closing=true;
        assert(fb_prepare_configuration(&request,&prepare_answer)==-1&&errno==E_PERM&&!begins&&!calls);
    }
    for(unsigned i=0;i<3;i++){
        prep_reset();kdisplay_prepared_configuration_t previous=prepare_answer;
        bad_read=i==0;bad_write=i==1;verdict=i==2?-E_BUSY:0;
        assert(fb_prepare_configuration(&request,&prepare_answer)==-1&&begins==1&&ends==1&&!held);
        assert(!memcmp(&prepare_answer,&previous,sizeof previous)&&calls==(i==2));
    }
    prep_reset();assert(!fb_cancel_configuration(expected_token)&&cancel_calls==1&&!checks&&begins==1&&ends==1);
    prep_reset();verdict=-E_BUSY;assert(fb_cancel_configuration(expected_token)==-1&&errno==E_BUSY&&ends==1);
    prep_reset();busy=true;assert(fb_prepare_configuration(&request,&prepare_answer)==-1&&errno==E_BUSY&&!checks&&!ends);
    prep_reset();native=false;assert(fb_cancel_configuration(expected_token)==-1&&errno==E_NOSYS&&!begins);
    prep_reset();assert(sys_fb_prepare_configuration(&process,999,0,0)==-E_INVAL&&begins==ends&&!calls&&!checks);
    puts("PASS prepare/cancel syscall: actual operations 13/14, nonrecycled owner, closing/foreign/zero owner, immutable/aliased request, write bounds, unchanged failure output, full-width token and balanced render guard");
}
'''
    code=code.replace('int main(void){',checks+'\nint main(void){')
    code=code.replace('    puts("PASS configuration syscall:', '    check_prepare_syscall();\n    puts("PASS configuration syscall:')
    code=re.sub(r'\blong\b','intptr_t',code)
    run_test(code,'display-prepare-syscall')

if __name__=='__main__':main()
