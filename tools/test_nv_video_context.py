#!/usr/bin/env python3
"""Execute production codec context bridge and binding with modeled RM."""
from pathlib import Path
from test_nv_external_tlb import function
from test_gpu_stable_candidate import run_test

ROOT = Path(__file__).resolve().parents[1]


def main():
    os = (ROOT / 'kernel/nvrm_os.c').read_text()
    chan = (ROOT / 'kernel/nv_chan.c').read_text()
    vendor = ROOT / 'out/nvidia-open-595.99.02/src/nvidia'
    flags = (vendor / 'generated/g_mem_desc_nvoc.h').read_text()
    assert '#define MEMDESC_FLAGS_GPU_PRIVILEGED               NVBIT64(13)' in flags
    falcon = (vendor / 'src/kernel/gpu/falcon/kernel_falcon.c').read_text()
    allocator = function(falcon, '_kflcnAllocAndMapCtxBuffer')
    assert 'MEMDESC_FLAGS_GPU_PRIVILEGED' not in allocator
    assert 'flags = MEMDESC_FLAGS_OWNED_BY_CURRENT_DEVICE' in allocator
    ops = (vendor / 'src/kernel/rmapi/nv_gpu_ops.c').read_text()
    assert 'privileged = memdescGetFlag(pMemDesc, MEMDESC_FLAGS_GPU_PRIVILEGED);' in ops
    assert function(chan, 'host_map_gr_resource').count('vram, read_only, true,') == 2
    c = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint8_t NvU8;typedef uint8_t NvBool;typedef uint32_t NvU32;
typedef uint64_t NvU64;typedef uint32_t NV_STATUS;typedef uint32_t NvHandle;
typedef uint32_t u32;typedef uint64_t u64;typedef uint16_t u16;typedef uint8_t u8;
#define NV_FALSE 0
#define NV_OK 0
#define NV_ERR_INVALID_ARGUMENT 1
#define NV_ERR_INVALID_OBJECT 2
#define NV_ERR_OBJECT_NOT_FOUND 3
#define NV_ERR_INVALID_DATA 4
#define NVRM_LOCK_MODULE_GPU 7
#define NVRM_NVOC_CLASS_KERNEL_CHANNEL 8
#define NVRM_KERNEL_CHANNEL_GROUP_API_OFFSET 0x2b8u
#define NVRM_CHANNEL_GROUP_FROM_API_OFFSET 0x138u
typedef struct {void *pResource;} nvrm_resource_ref_prefix_t;
static unsigned mode,api_locks,gpu_locks,flag_reads;
static bool nvrm_adapter_ready=true,descriptor_priv;
static u64 descriptor[64],channel_storage[128],group_storage[64];
static int gpu_object,group_object;
static nvrm_resource_ref_prefix_t ref;
NV_STATUS rmapiLockAcquire(NvU32 a,NvU32 b){assert(!a&&b==7&&!api_locks&&!gpu_locks);if(mode==1)return 91;api_locks++;return 0;}
void rmapiLockRelease(void){assert(api_locks==1&&!gpu_locks);api_locks--;}
NV_STATUS rmGpuLocksAcquire(NvU32 a,NvU32 b){assert(!a&&b==7&&api_locks);if(mode==2)return 92;gpu_locks++;return 0;}
NvU32 rmGpuLocksRelease(NvU32 a,void *b){assert(!a&&!b&&api_locks&&gpu_locks==1);gpu_locks--;return 0;}
NV_STATUS serverutilGetResourceRefWithType(NvHandle c,NvHandle h,NvU32 t,void **out){
    assert(c==2&&h==3&&t==8&&api_locks&&gpu_locks);if(mode==3)return 93;*out=mode==4?NULL:&ref;return 0;}
void *objDynamicCastById_IMPL(void *p,NvU32 t){assert(p==channel_storage&&t==8);return mode==5?NULL:p;}
void *gpumgrGetSomeGpu(void){return mode==6?NULL:&gpu_object;}
NV_STATUS kchangrpGetEngineContextMemDesc_IMPL(void *g,void *group,void **out){
    assert(g==&gpu_object&&group==&group_object&&api_locks&&gpu_locks);if(mode==9)return 99;
    *out=mode==10?NULL:descriptor;return 0;}
NvU64 memdescGetPhysAddr(void *d,void *at,NvU64 off){assert(d==descriptor&&at==(void*)1&&!off);return 0x1df0000;}
NvBool memdescGetContiguity(void *d,void *at){assert(d==descriptor&&at==(void*)1);return mode!=12;}
NvU64 memdescGetPageSize(void *d,void *at){assert(d==descriptor&&at==(void*)1);return mode==11?1ull<<32:4096;}
NvU32 memdescGetAddressSpace(void *d){assert(d==descriptor);return mode==13?3:2;}
NvU32 memdescGetPteKindForGpu(void *d,void *g){assert(d==descriptor&&g==&gpu_object);return 0;}
NvBool memdescGetFlag(void *d,NvU64 f){assert(d==descriptor&&f==(1ull<<13)&&api_locks&&gpu_locks);flag_reads++;return descriptor_priv;}
static void fixture(void){
    memset(descriptor,0,sizeof descriptor);memset(channel_storage,0,sizeof channel_storage);
    memset(group_storage,0,sizeof group_storage);ref.pResource=channel_storage;
    *(void **)((NvU8*)channel_storage+0x2b8)=mode==7?NULL:group_storage;
    *(void **)((NvU8*)group_storage+0x138)=mode==8?NULL:&group_object;
    descriptor[0x28/8]=4096;descriptor[0x30/8]=65536;
}
'''
    c += '\n' + function(os, 'nvrm_host_get_video_falcon_context')
    c += r'''
#define PAGE_SIZE 4096
#define VA_HOST_CTX_BASE 0x400000000ull
#define VA_HOST_CTX_ALIGN 0x8000000ull
#define HOST_GR_CTX_MAX 16
#define NV2080_CTRL_CMD_GR_GET_CTX_BUFFER_INFO 1
#define NV2080_CTRL_CMD_GPU_PROMOTE_CTX 2
#define kerr(...) ((void)0)
#define kinfo(...) ((void)0)
typedef struct {int unused;} nv_card_t;
typedef struct {u32 client;} nv_rm_t;
typedef struct {int unused;} nv_vmm_t;
typedef struct {nv_vmm_t vmm;u32 h_channel;const char *name;} nv_channel_t;
typedef struct {u64 alignment,size;u32 buffer_type,page_size,aperture,kind;} host_gr_ctx_info_t;
typedef struct {u32 h_user_client,h_channel,buffer_count;host_gr_ctx_info_t info[16];} host_gr_ctx_info_params_t;
typedef struct {u32 h_user_client,h_channel;u64 alignment,size,phys_addr;u32 aperture,kind,page_size;bool is_contiguous;} host_flcn_ctx_info_params_t;
typedef struct {u64 gpu_virt_addr;u16 buffer_id;} entry_t;
typedef struct {u32 engine_type,h_chan_client,h_object,entry_count;entry_t entry[16];} promote_ctx_params_t;
static u32 nv_last_control_status;
static unsigned mapped,committed,promoted,bound,fail_stage;
static bool mapped_priv;
static bool host_map_gr_resource(nv_card_t *c,nv_rm_t *r,nv_channel_t *ch,const host_gr_ctx_info_t *i,u64 v){
    (void)c;(void)r;(void)ch;(void)i;(void)v;assert(false);return false;}
static u32 h_vaspace(int i){return (u32)i+100;}
static bool nv_vmm_map_kind(nv_vmm_t *v,u64 va,u64 pa,u64 size,bool vram,bool ro,bool priv,u32 kind){
    (void)v;assert(va==VA_HOST_CTX_BASE&&pa==0x1df0000&&size==65536&&vram&&!ro&&!kind);
    assert(!committed&&!promoted&&!bound);mapped++;mapped_priv=priv;return fail_stage!=1;}
static bool nv_vmm_commit(nv_card_t *c,nv_rm_t *r,nv_vmm_t *v,u32 h,const char *label){
    (void)c;(void)r;(void)v;(void)label;assert(mapped==1&&!promoted&&!bound&&(h==103||h==104));committed++;return fail_stage!=2;}
static bool nv_rm_control(nv_card_t *c,nv_rm_t *r,u32 h,u32 cmd,void *data,u32 size,void *out,u32 out_size,u32 *got){
    (void)c;(void)r;(void)h;assert(cmd==2&&size==sizeof(promote_ctx_params_t)&&!out&&!out_size&&!got);
    promote_ctx_params_t *p=data;assert(p->h_chan_client==2&&p->h_object==3&&p->entry_count==1);
    assert(p->entry[0].gpu_virt_addr==VA_HOST_CTX_BASE&&committed==1&&!bound);promoted++;return fail_stage!=3;}
#define RM_SUBDEVICE 9
static u32 nvrm_host_mark_context_bound(u32 c,u32 h){assert(c==2&&h==3&&promoted==1);bound++;return fail_stage==4?1:0;}
'''
    c += '\n' + function(chan, 'align_up_u64')
    c += '\n' + function(chan, 'host_ctx_aperture')
    c += '\n' + function(chan, 'host_bind_channel_resources')
    c += r'''
int main(void){
    nv_card_t card={0};nv_rm_t rm={2};nv_channel_t ch={.h_channel=3,.name="codec"};
    for(unsigned priv=0;priv<2;priv++)for(int engine=3;engine<=4;engine++) {
        mode=0;fixture();descriptor_priv=priv;mapped=committed=promoted=bound=0;
        assert(host_bind_channel_resources(&card,&rm,&ch,engine,engine,false));
        assert(mapped==1&&committed==1&&promoted==1&&bound==1&&mapped_priv==descriptor_priv);
        assert(!api_locks&&!gpu_locks);
    }
    for(mode=1;mode<=13;mode++) {
        fixture();mapped=committed=promoted=bound=0;
        assert(!host_bind_channel_resources(&card,&rm,&ch,3,3,false));
        assert(!mapped&&!committed&&!promoted&&!bound&&!api_locks&&!gpu_locks);
    }
    mode=0;fixture();
    for(fail_stage=1;fail_stage<=4;fail_stage++) {
        mapped=committed=promoted=bound=0;
        assert(!host_bind_channel_resources(&card,&rm,&ch,3,3,false));
        assert(mapped==1&&committed==(fail_stage>=2)&&promoted==(fail_stage>=3)&&bound==(fail_stage>=4));
        assert(!api_locks&&!gpu_locks);
    }
    NvU64 a,s,p;NvU32 ap,k,ps;NvBool contiguous;
    assert(nvrm_host_get_video_falcon_context(2,3,&a,&s,&p,&ap,&k,&ps,&contiguous,NULL)==NV_ERR_INVALID_ARGUMENT);
    assert(flag_reads);
    puts("PASS actual codec context bridge/binding: RM privilege preserved for both codecs, 13 lookup/attribute failures, mapping/commit/promotion ordering, balanced locks; GR permissions unchanged (modeled RM)");
}
'''
    run_test(c, 'video_context')


if __name__ == '__main__':
    main()
