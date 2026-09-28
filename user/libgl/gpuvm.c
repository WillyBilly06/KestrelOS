#include "gpuvm.h"
#include "../../include/kestrel/gpu2d.h"

#define VM_BUFFER_MAX (256ull*1024u*1024u)
#define VM_VERTEX_UPLOAD_MAX (128u*1024u)
#define VM_VERTEX_SHADOW_MAX (16u*1024u*1024u)

static void vm_vertex_invalidate(gl_gpu_vm_t *vm) {
    memset(vm->vertex_shadow_valid,0,sizeof vm->vertex_shadow_valid);
}

static bool vm_request(gl_gpu_vm_t *vm, kg2d_request_t *r) {
    if (vm->quarantined) return false;
    r->version=KG2D_ABI;
    intptr_t error;
    unsigned attempts=0;
    do {
        error=syscall6(SYS_GPU,GPUOP_SURFACE,(intptr_t)r,sizeof *r,0,0,0);
        if(error!=-EBUSY || ++attempts==32u)break;
        /* EBUSY is returned before the request is copied or submitted. A
         * background RM reader must not permanently disable the 3D window.
         * Yield between bounded retries; never retry a submitted/failed job. */
        sleep_ms(1);
    } while(true);
    vm->last_error=error<0?error:0;
    if(error>=0)return true;
    vm_vertex_invalidate(vm);
    vm->ready=false;
    /* EIO may mean an unfenced engine or failed mapping: never retry it. */
    if(error==-EIO)vm->quarantined=true;
    if(error!=-EBUSY){
        char note[192];
        snprintf(note,sizeof note,"shader transport op=%u error=%lld quarantined=%u; output invalid",
                 r->operation,(long long)error,vm->quarantined);
        log_write(3,"gl-vm",note);
    }
    return false;
}

static bool vm_drop(gl_gpu_vm_t *vm, gl_gpu_vm_buffer_t *buffer) {
    if(!buffer->handle)return true;
    kg2d_request_t r={.operation=KG2D_DESTROY,.handle=buffer->handle};
    if(!vm_request(vm,&r))return false;
    *buffer=(gl_gpu_vm_buffer_t){0};return true;
}

static bool vm_buffer(gl_gpu_vm_t *vm, gl_gpu_vm_buffer_t *buffer, uint64_t bytes) {
    if(bytes>VM_BUFFER_MAX){vm->last_error=-EINVAL;return false;}
    if(bytes && buffer->handle && buffer->bytes>=bytes)return true;
    if(buffer==&vm->registers)vm_vertex_invalidate(vm);
    /* Growth/release happens only between retired calls. Dropping first avoids
     * transiently consuming double the memory and an extra surface slot. */
    if(!vm_drop(vm,buffer))return false;
    if(!bytes)return true;
    kg2d_request_t r={.operation=KG2D_CREATE,.width=4096,
                     .height=(unsigned)((bytes+16383u)/16384u)};
    if(!vm_request(vm,&r))return false;
    buffer->handle=r.handle;buffer->bytes=r.bytes;
    if(!r.handle || r.bytes<bytes || r.bytes>VM_BUFFER_MAX){
        vm->ready=false;vm->last_error=-EIO;vm->quarantined=true;return false;
    }
    return true;
}

static bool vm_transfer(gl_gpu_vm_t *vm, const gl_gpu_vm_buffer_t *buffer,
                        uint64_t offset, void *data, uint64_t bytes, bool read) {
    if(!buffer->handle || !data || !bytes || ((offset|bytes)&3u) ||
       offset>buffer->bytes || bytes>buffer->bytes-offset){vm->ready=false;vm->last_error=-EINVAL;return false;}
    unsigned char *p=data;
    while(bytes){
        unsigned chunk=bytes>KG2D_TRANSFER_MAX?KG2D_TRANSFER_MAX:(unsigned)bytes;
        kg2d_request_t r={.operation=read?KG2D_DOWNLOAD:KG2D_UPLOAD,.handle=buffer->handle,
                         .offset=offset,.data=(uint64_t)(uintptr_t)p,.bytes=chunk};
        if(!vm_request(vm,&r))return false;
        p+=chunk;offset+=chunk;bytes-=chunk;
    }
    return true;
}

static bool vm_vertex_upload(gl_gpu_vm_t *vm, const ksh_dispatch_t *job,
                             const float *registers) {
    bool used[SR_REGISTERS];gl_gpu_vertex_registers(job,used);
    unsigned first=0,last=SR_REGISTERS;
    while(first<last&&!used[first])first++;
    while(last>first&&!used[last-1])last--;
    uint64_t plane=(uint64_t)job->lanes*16u;
    uint64_t bytes=(last-first)*plane;
    uint64_t shadow_bytes=plane*SR_REGISTERS;
    bool cache=false;
    /* Small draws already take one packed upload. Do not replace that with
     * multiple tiny dirty transfers. Larger batches benefit from retaining
     * unchanged attributes/constants instead of retransferring them per frame. */
    /* A single bank cannot retain two different SoA strides. Wait for a
     * repeated layout before allocating/copying a shadow; alternating mesh
     * and ground draws must not allocate multi-megabyte shadows every frame. */
    if(vm->vertex_shadow_lanes==job->lanes &&
       bytes>VM_VERTEX_UPLOAD_MAX && shadow_bytes<=VM_VERTEX_SHADOW_MAX){
        if(vm->vertex_shadow_capacity<shadow_bytes ||
           vm->vertex_shadow_capacity/4u>=shadow_bytes){
            /* This shadow is only an optimization. Release before resizing
             * rather than transiently retaining two multi-megabyte copies. */
            free(vm->vertex_shadow);vm->vertex_shadow=NULL;
            vm->vertex_shadow_capacity=0;vm_vertex_invalidate(vm);
            unsigned char *shadow=malloc((size_t)shadow_bytes);
            if(shadow){
                vm->vertex_shadow=shadow;
                vm->vertex_shadow_capacity=(unsigned)shadow_bytes;
            }
        }
        cache=vm->vertex_shadow_capacity>=shadow_bytes;
    }else if(vm->vertex_shadow){
        free(vm->vertex_shadow);vm->vertex_shadow=NULL;
        vm->vertex_shadow_capacity=0;
    }
    if(!cache || vm->vertex_shadow_lanes!=job->lanes)vm_vertex_invalidate(vm);
    vm->vertex_shadow_lanes=job->lanes;
    /* Each separate UPLOAD is a synchronous copy-engine transaction. For a
     * small mesh, packing initialized gaps is cheaper in transaction count
     * than uploading the scattered attributes, uniforms, temporaries and
     * outputs separately. Large meshes keep sparse transfers so this does not
     * turn terrain into a full-register-file upload. */
    if(bytes && bytes<=VM_VERTEX_UPLOAD_MAX){
        if(vm->vertex_upload_capacity<bytes){
            unsigned char *upload=malloc((size_t)bytes);
            if(upload){
                free(vm->vertex_upload);vm->vertex_upload=upload;
                vm->vertex_upload_capacity=(unsigned)bytes;
            }
        }
        /* Staging is optional: an allocation failure preserves the existing
         * sparse path instead of failing an otherwise renderable frame. */
        if(vm->vertex_upload_capacity>=bytes){
            memset(vm->vertex_upload,0,(size_t)bytes);
            for(unsigned r=first;r<last;r++)if(used[r])
                memcpy(vm->vertex_upload+(r-first)*plane,
                       registers+(uint64_t)r*job->lanes*4u,(size_t)plane);
            return vm_transfer(vm,&vm->registers,first*plane,
                               vm->vertex_upload,bytes,false);
        }
    }
    bool dirty[SR_REGISTERS];
    for(unsigned r=0;r<SR_REGISTERS;r++)
        dirty[r]=used[r] && (!cache || !vm->vertex_shadow_valid[r] ||
            memcmp(vm->vertex_shadow+(uint64_t)r*plane,
                   registers+(uint64_t)r*job->lanes*4u,(size_t)plane));
    for(unsigned r=first;r<last;){
        if(!dirty[r]){r++;continue;}
        unsigned end=r+1;while(end<last&&dirty[end])end++;
        if(!vm_transfer(vm,&vm->registers,r*plane,
                        (void*)(registers+(uint64_t)r*job->lanes*4u),
                        (end-r)*plane,false))return false;
        if(cache){
            memcpy(vm->vertex_shadow+(uint64_t)r*plane,
                   registers+(uint64_t)r*job->lanes*4u,(size_t)((end-r)*plane));
            for(unsigned k=r;k<end;k++)vm->vertex_shadow_valid[k]=true;
        }
        r=end;
    }
    return true;
}

static bool vm_execute(gl_gpu_vm_t *vm, const ksh_dispatch_t *job,
                         const float *registers, uint64_t register_floats,
                         const void *textures, uint64_t texture_bytes, bool resident, bool vertex) {
    if(!vm)return false;
    vm->ready=false;vm->raster_output=false;vm->vertex_output=vertex;vm->lanes=0;vm->lane_error=0;vm->bad_lane=0;
    if(vm->quarantined)return false;
    if(!resident)vm->texture_valid=false;
    vm->last_error=-EINVAL;
    if(!job || !registers || !job->lanes || job->lanes>KSH_MAX_LANES ||
       !job->code_count || job->code_count>SH_MAX_INSTRUCTIONS ||
       !job->budget || job->budget>KSH_MAX_STEPS ||
       (uint64_t)job->lanes*job->budget>KSH_MAX_WORK ||
       job->texture_count>KSH_MAX_TEXTURES ||
        texture_bytes>VM_BUFFER_MAX || (texture_bytes&3u) ||
        (resident?!vm->texture_valid:(texture_bytes&&!textures)))return false;
    unsigned lanes=job->lanes,budget=job->budget;
    uint64_t floats=(uint64_t)lanes*SR_REGISTERS*4u;
    if(register_floats!=floats)return false;
    if(vm->result_capacity<lanes){
        ksh_status_t *results=malloc((size_t)lanes*sizeof(*results));
        if(!results){vm->last_error=-ENOMEM;return false;}
        free(vm->results);vm->results=results;vm->result_capacity=lanes;
    }
    if(!vm_buffer(vm,&vm->registers,floats*sizeof(float)) ||
       !vm_buffer(vm,&vm->status,(uint64_t)lanes*sizeof(ksh_status_t)) ||
       !vm_buffer(vm,&vm->textures,texture_bytes))return false;
    if(vertex){
        // Address-free bytecode has only explicit register operands, except for
        // MATMUL's four adjacent columns. Include destinations for partial writes
        // and every branch/loop body: no data-dependent CPU shader execution.
        if(!vm_vertex_upload(vm,job,registers))return false;
    }else{
        vm_vertex_invalidate(vm);
        if(!vm_transfer(vm,&vm->registers,0,(void *)registers,floats*sizeof(float),false))return false;
    }
    if(!resident){
        vm->texture_valid=false;
        if(texture_bytes&&!vm_transfer(vm,&vm->textures,0,(void *)textures,texture_bytes,false))return false;
        vm->texture_valid=true;vm->texture_bytes=texture_bytes;
    }
    kg2d_request_t r={.operation=KG2D_SHADER_VM,.handle=vm->registers.handle,
                     .offset=vm->status.handle,.source=vm->textures.handle,
                     .data=(uint64_t)(uintptr_t)job,.bytes=sizeof(*job)};
    /* Consider all bytecode destinations, including partial writes and untaken
     * branches. Program identity and pointers are not immutability proofs.
     * Invalidate before dispatch, so a failure cannot retain an output plane. */
    if(vertex){
        vm->vertex_shadow_valid[SR_POSITION]=false;
        for(unsigned v=SR_VARYING;v<SR_VARYING+SR_VARYING_N;v++)
            vm->vertex_shadow_valid[v]=false;
        for(unsigned pc=0;pc<job->code_count;pc++)
            if(job->code[pc].dst<SR_REGISTERS)
                vm->vertex_shadow_valid[job->code[pc].dst]=false;
    }
    if(!vm_request(vm,&r))return false; // never read status from an unfenced launch
    memset(vm->results,0,(size_t)lanes*sizeof(*vm->results));
    if(!vm_transfer(vm,&vm->status,0,vm->results,(uint64_t)lanes*sizeof(*vm->results),true))return false;
    for(unsigned i=0;i<lanes;i++){
        const ksh_status_t *s=&vm->results[i];
        if((s->result!=KSH_COMPLETE&&s->result!=KSH_DISCARDED) ||
           !s->executed || s->executed>budget){
            vm->last_error=-EIO;vm->bad_lane=i;vm->lane_error=s->result;
            vm_vertex_invalidate(vm);
            char note[192];snprintf(note,sizeof note,
                "shader lane=%u result=%u executed=%u budget=%u; fenced but output rejected",
                i,s->result,s->executed,budget);
            log_write(3,"gl-vm",note);
            return false; // fenced logical failure: buffers may be reused, output may not
        }
    }
    vm->lanes=lanes;vm->ready=true;vm->last_error=0;return true;
}

bool gl_gpu_vm_execute(gl_gpu_vm_t *vm, const ksh_dispatch_t *job,
                        const float *registers, uint64_t register_floats,
                        const void *textures, uint64_t texture_bytes) {
    return vm_execute(vm,job,registers,register_floats,textures,texture_bytes,false,false);
}
bool gl_gpu_vm_set_textures(gl_gpu_vm_t *vm, const void *textures, uint64_t bytes) {
    if(!vm || vm->quarantined)return false;
    vm->ready=false;vm->texture_valid=false;vm->last_error=-EINVAL;
    if(bytes>VM_BUFFER_MAX || (bytes&3u) || (bytes&&!textures))return false;
    if(!vm_buffer(vm,&vm->textures,bytes) ||
       (bytes&&!vm_transfer(vm,&vm->textures,0,(void*)textures,bytes,false)))return false;
    vm->texture_bytes=bytes;vm->texture_valid=true;vm->last_error=0;return true;
}
bool gl_gpu_vm_execute_resident(gl_gpu_vm_t *vm, const ksh_dispatch_t *job,
                                const float *registers, uint64_t register_floats) {
    return vm_execute(vm,job,registers,register_floats,NULL,vm?vm->texture_bytes:0,true,false);
}
bool gl_gpu_vm_execute_vertex(gl_gpu_vm_t *vm, const ksh_dispatch_t *job,
                              const float *registers, uint64_t register_floats) {
    return vm_execute(vm,job,registers,register_floats,NULL,vm?vm->texture_bytes:0,true,true);
}
bool gl_gpu_vm_raster(gl_gpu_vm_t *vm, uint64_t destination, uint64_t depth,
                      const kshr_job_t *job) {
    if(!vm)return false;
    vm->ready=false;vm->raster_output=true;vm->lanes=0;vm->lane_error=0;vm->bad_lane=0;
    if(vm->quarantined)return false;
    vm->last_error=-EINVAL;
    if(!job || !destination || !vm->texture_valid || !job->clip_w || !job->clip_h ||
       (uint64_t)job->clip_w*job->clip_h>KSH_MAX_WORK ||
       !job->command_count || job->command_count>KG3D_MAX_COMMANDS ||
       !job->code_count || job->code_count>SH_MAX_INSTRUCTIONS ||
       !job->budget || job->budget>KSH_MAX_STEPS || job->texture_count>KSH_MAX_TEXTURES)return false;
    uint64_t pixels=(uint64_t)job->clip_w*job->clip_h;
    unsigned lanes=kshr_job_lanes(job),work=0;
    for(unsigned i=0;i<job->command_count;i++)work+=(job->commands[i].raster.flags&KG3D_LINE)?KG3D_LINE_WORK:1u;
    unsigned limit=work*job->budget;
    if(kshr_region_cost(job->commands,job->command_count,job->budget,
        job->clip_x,job->clip_y,job->clip_w,job->clip_h)>KSH_MAX_WORK)return false;
    if(!vm->raster_packet){
        vm->raster_packet=malloc(sizeof(*vm->raster_packet));
        if(!vm->raster_packet){vm->last_error=-ENOMEM;return false;}
    }
    if(vm->result_capacity<lanes){
        ksh_status_t *s=malloc((size_t)lanes*sizeof(*s));
        if(!s){vm->last_error=-ENOMEM;return false;}
        free(vm->results);vm->results=s;vm->result_capacity=lanes;
    }
    /* Keep vertex planes intact across fragment passes. Allocate only the
     * selected raster variant's required scratch, lazily. If another surface
     * cannot be allocated before submission, retain the old shared-scratch
     * route and invalidate every cached vertex plane before it is overwritten. */
    bool preserve_vertex=false;
    for(unsigned r=0;r<SR_REGISTERS;r++)preserve_vertex|=vm->vertex_shadow_valid[r];
    gl_gpu_vm_buffer_t *scratch=preserve_vertex?&vm->raster_scratch:&vm->registers;
    if(!preserve_vertex){
        vm_vertex_invalidate(vm);
        /* No retained inputs to protect: keep the old one-surface path and
         * release an obsolete dedicated allocation after its last fence. */
        if(!vm_drop(vm,&vm->raster_scratch))return false;
    }
    uint64_t scratch_bytes=KSHR_JOB_ALLOCATION_BYTES(job);
    /* A switch from the general VM to the register-fast fragment path no
     * longer needs megabytes of per-lane scratch. Release after retirement. */
    if(preserve_vertex && scratch->bytes/4u>=scratch_bytes && !vm_drop(vm,scratch))return false;
    if(!vm_buffer(vm,scratch,scratch_bytes)){
        if(!preserve_vertex || vm->quarantined ||
           (vm->last_error!=-ENOMEM && vm->last_error!=-ENOSPC))return false;
        scratch=&vm->registers;
        vm_vertex_invalidate(vm);
        if(!vm_buffer(vm,scratch,scratch_bytes))return false;
    }
    if(!vm_buffer(vm,&vm->status,(uint64_t)lanes*sizeof(ksh_status_t)))return false;
    vm->raster_packet->scratch=scratch->handle;
    vm->raster_packet->status=vm->status.handle;
    memcpy(&vm->raster_packet->job,job,sizeof(*job));
    kg2d_request_t r={.operation=KG2D_SHADER_RASTER,.handle=destination,.offset=depth,
        .source=vm->textures.handle,.data=(uint64_t)(uintptr_t)vm->raster_packet,.bytes=sizeof(*vm->raster_packet)};
    if(!vm_request(vm,&r))return false;
    memset(vm->results,0,(size_t)lanes*sizeof(*vm->results));
    if(!vm_transfer(vm,&vm->status,0,vm->results,(uint64_t)lanes*sizeof(*vm->results),true))return false;
    for(unsigned i=0;i<lanes;i++){
      unsigned lane_limit=(unsigned)((1u+(pixels-1u-i)/lanes)*limit);
      if(vm->results[i].result!=KSH_COMPLETE || vm->results[i].executed>lane_limit){
        vm->last_error=-EIO;vm->bad_lane=i;vm->lane_error=vm->results[i].result;
        vm_vertex_invalidate(vm);
        char note[192];snprintf(note,sizeof note,"raster lane=%u result=%u executed=%u limit=%u; reject entire batch",
            i,vm->results[i].result,vm->results[i].executed,lane_limit);log_write(3,"gl-vm",note);return false;
      }
    }
    vm->lanes=lanes;vm->ready=true;vm->last_error=0;return true;
}

bool gl_gpu_vm_geometry(gl_gpu_vm_t *vm,uint64_t destination,uint64_t depth,
                        const kshs_config_t *setup,const kshr_job_t *fragment) {
    if(!vm)return false;
    bool vertex_ready=vm->ready && vm->vertex_output && !vm->raster_output;
    unsigned vertex_lanes=vm->lanes;
    vm->ready=false;vm->raster_output=true;vm->vertex_output=false;vm->lanes=0;
    vm->geometry_triangles=0;vm->geometry_instructions=0;
    vm->lane_error=0;vm->bad_lane=0;
    if(vm->quarantined)return false;
    vm->last_error=-EINVAL;
    kshs_storage_layout_t layout;
    if(!vertex_ready || !destination || !setup || !fragment ||
       !vm->texture_valid || setup->version!=KSHS_ABI || setup->lanes!=vertex_lanes ||
       setup->first_vertex>vertex_lanes ||
       setup->triangle_count>(vertex_lanes-setup->first_vertex)/3u ||
       !kshs_storage_layout(setup->triangle_count,&layout) ||
       setup->clip_w<=0 || setup->clip_h<=0 ||
       !fragment->code_count || fragment->code_count>SH_MAX_INSTRUCTIONS ||
       !fragment->budget || fragment->budget>KSH_MAX_STEPS ||
       fragment->varying_count>SR_VARYING_N || fragment->texture_count>KSH_MAX_TEXTURES)
        return false;
    if(!vm->geometry_packet){
        vm->geometry_packet=malloc(sizeof(*vm->geometry_packet));
        if(!vm->geometry_packet){vm->last_error=-ENOMEM;return false;}
    }
    kshs_submission_t *packet=vm->geometry_packet;
    packet->setup=*setup;
    memcpy(&packet->fragment,fragment,sizeof(*fragment));
    packet->fragment.command_count=0; // GPU setup produces every triangle record
    packet->fragment.clip_x=setup->clip_x;packet->fragment.clip_y=setup->clip_y;
    packet->fragment.clip_w=setup->clip_w;packet->fragment.clip_h=setup->clip_h;
    unsigned lanes=kshr_job_lanes(&packet->fragment);
    uint64_t scratch=KSHR_JOB_ALLOCATION_BYTES(&packet->fragment);
    /* Every previously submitted operation has retired. Never reuse vertex
     * registers or their completion records as writable setup/raster scratch. */
    if(!vm_buffer(vm,&vm->geometry_workspace,layout.bytes) ||
       !vm_buffer(vm,&vm->raster_scratch,scratch) ||
       !vm_buffer(vm,&vm->geometry_status,(uint64_t)lanes*sizeof(ksh_status_t)))return false;
    packet->registers=vm->registers.handle;packet->vertex_status=vm->status.handle;
    packet->workspace=vm->geometry_workspace.handle;
    packet->raster_scratch=vm->raster_scratch.handle;
    packet->raster_status=vm->geometry_status.handle;
    kg2d_request_t r={.operation=KG2D_SHADER_GEOMETRY,.handle=destination,.offset=depth,
        .source=vm->textures.handle,.data=(uint64_t)(uintptr_t)packet,.bytes=sizeof(*packet)};
    if(!vm_request(vm,&r))return false;
    if(r.count>setup->triangle_count*KSHS_OUTPUT_TRIANGLES || r.bytes>KSHS_MAX_RASTER_WORK){
        vm->last_error=-EIO;vm->quarantined=true;vm_vertex_invalidate(vm);return false;
    }
    vm->geometry_triangles=r.count;
    vm->geometry_instructions=r.bytes; // validated retired fragment instructions, not elapsed time
    vm->last_error=0;vm->ready=true;return true;
}

bool gl_gpu_vm_read(gl_gpu_vm_t *vm, unsigned first, unsigned count,
                    float *out, uint64_t output_floats) {
    if(!vm || vm->quarantined)return false;
    if(!vm->ready){if(!vm->last_error)vm->last_error=-EINVAL;return false;}
    if(vm->raster_output || !out || !count || first>=SR_REGISTERS ||
       count>SR_REGISTERS-first || output_floats!=(uint64_t)count*4u*vm->lanes ||
       (vm->vertex_output && !((first==SR_POSITION&&count==1) ||
        (first>=SR_VARYING&&first<SR_VARYING+SR_VARYING_N&&count<=SR_VARYING+SR_VARYING_N-first)))){
        vm->ready=false;vm->last_error=-EINVAL;return false;
    }
    uint64_t offset=(uint64_t)first*4u*vm->lanes*sizeof(float);
    return vm_transfer(vm,&vm->registers,offset,out,output_floats*sizeof(float),true);
}

bool gl_gpu_vm_release(gl_gpu_vm_t *vm) {
    if(!vm)return false;
    vm->ready=false;vm->lanes=0;
    free(vm->results);vm->results=NULL;vm->result_capacity=0;
    free(vm->raster_packet);vm->raster_packet=NULL;vm->texture_valid=false;vm->texture_bytes=0;
    free(vm->vertex_upload);vm->vertex_upload=NULL;vm->vertex_upload_capacity=0;
    free(vm->vertex_shadow);vm->vertex_shadow=NULL;vm->vertex_shadow_capacity=0;
    vm->vertex_shadow_lanes=0;vm_vertex_invalidate(vm);
    free(vm->geometry_packet);vm->geometry_packet=NULL;
    vm->geometry_triangles=0;vm->geometry_instructions=0;
    if(vm->quarantined)return false; // preserve handles; kernel quarantine owns their lifetime
    if(!vm_drop(vm,&vm->textures) || !vm_drop(vm,&vm->status) ||
       !vm_drop(vm,&vm->raster_scratch) || !vm_drop(vm,&vm->geometry_status) ||
       !vm_drop(vm,&vm->geometry_workspace) || !vm_drop(vm,&vm->registers))return false;
    vm->last_error=0;return true;
}
