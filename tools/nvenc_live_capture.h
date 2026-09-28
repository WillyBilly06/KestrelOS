/* Windows 610.62, self-process-only observation of typed NVENC commands.
 * SHA-pinned by generate_nvenc_h264_fixture.py; exact entry bytes checked here.
 * No other process, BAR, device register, or installed file is modified/read.
 * The temporary breakpoint replaces one byte in this process's private DLL
 * mapping. Its handler emulates ONLY the verified MOV R10,[RCX+8] instruction;
 * all encoder arguments, flags and subsequent instructions are unchanged.
 */
#ifndef KESTREL_NVENC_LIVE_CAPTURE_H
#define KESTREL_NVENC_LIVE_CAPTURE_H
struct NvLiveToken {
    unsigned method,type,flags,data,va_getter_rva,size_getter_rva,slot38_value,address_metadata_valid;
    unsigned long long resource,gpu_va;
};
struct NvLiveBatch { unsigned count,original_count,execute_data; NvLiveToken tokens[256]; };
static NvLiveBatch nv_live_batches[8];
static LONG nv_live_count,nv_live_dropped;
static unsigned char *nv_live_site;
static DWORD nv_live_protection;
static void *nv_live_handler;
static uintptr_t nv_live_image;
static size_t nv_live_image_size;
static unsigned char *nv_rc_init_site;
static DWORD nv_rc_init_protection;
static LONG nv_rc_init_count;
static unsigned char nv_rc_init_records[8][256];
static bool nv_rc_init_valid[8];
static bool nv_live_remove();

static bool nv_live_read(uintptr_t p,void *out,size_t bytes){
    size_t got=0;return read_owned_data(GetCurrentProcess(),p,out,bytes,&got);
}
static unsigned nv_live_getter(uintptr_t resource,unsigned slot){
    uintptr_t table=0,fn=0;size_t got=0;
    if(!nv_live_read(resource,&table,8) || table<nv_live_image ||
       table-nv_live_image>nv_live_image_size || slot+8>nv_live_image_size-(table-nv_live_image))return 0;
    // Targeted vtable entry inside the exact SHA-pinned module, not code/page scanning.
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(table+slot),&fn,8,&got) || got!=8 ||
       fn<nv_live_image || fn-nv_live_image>=nv_live_image_size)return 0;
    return unsigned(fn-nv_live_image);
}
static void nv_live_observe(uintptr_t descriptor,unsigned execute_data){
    unsigned char desc[16];
    if(!nv_live_read(descriptor,desc,sizeof desc))return;
    uint32_t count=0;uintptr_t list=0;
    std::memcpy(&count,desc,4);std::memcpy(&list,desc+8,8);
    if(count<9 || count>256 || !list)return;
    unsigned char raw[256*24];
    if(!nv_live_read(list,raw,size_t(count)*24))return;
    unsigned required=0;
    for(unsigned i=0;i<count;i++){
        unsigned char *p=raw+i*24;uint16_t method=0;uint32_t data=0;uintptr_t resource=0;
        std::memcpy(&method,p,2);std::memcpy(&data,p+4,4);std::memcpy(&resource,p+8,8);
        if(p[2]==0){
            if(method==0x200 && data==1)required|=1;
            if(method==0x700 && (data&15)==3)required|=2;
            if(method==0x704 && data==0)required|=4;
        }else if(p[2]==4 && resource){
            if(method==0x710)required|=8;
            if(method==0x718)required|=16;
            if(method==0x71c)required|=32;
            if(method==0x734)required|=64;
            if(method==0x740)required|=128;
            if(method==0x744)required|=256;
        }
    }
    if(required!=511)return; // Not our first H264 picture's complete binding list.
    LONG slot=InterlockedIncrement(&nv_live_count)-1;
    if(slot>=8){InterlockedIncrement(&nv_live_dropped);return;}
    NvLiveBatch &batch=nv_live_batches[slot];
    batch.original_count=count;batch.execute_data=execute_data;
    for(unsigned i=0;i<count;i++){
        unsigned char *p=raw+i*24;
        // Only decode the three source-verified token layouts. Do not dump
        // uninitialized token padding or unknown objects/command payloads.
        NvLiveToken &t=batch.tokens[batch.count++];uint16_t method=0;
        std::memcpy(&method,p,2);t.method=method;t.type=p[2];
        if(t.type==0 || t.type==4 || t.type==7)std::memcpy(&t.data,p+4,4);
        if(t.type==4){
            t.flags=p[3]&1;std::memcpy(&t.resource,p+8,8);
            t.va_getter_rva=nv_live_getter(uintptr_t(t.resource),0x30);
            t.size_getter_rva=nv_live_getter(uintptr_t(t.resource),0x38);
            /* SHA-pinned getters: 57f0 MOV RAX,[RCX+f8]; 58e0 MOV EAX,[RCX+78].
             * Never invoke an arbitrary virtual function from the observer. */
            if(t.va_getter_rva==0x57f0 && t.size_getter_rva==0x58e0 && t.resource<=UINTPTR_MAX-0x100){
                t.address_metadata_valid=nv_live_read(uintptr_t(t.resource)+0xf8,&t.gpu_va,8) &&
                    nv_live_read(uintptr_t(t.resource)+0x78,&t.slot38_value,4);
            }
        }
    }
}
static LONG CALLBACK nv_live_exception(EXCEPTION_POINTERS *e){
    if(nv_rc_init_site && e->ExceptionRecord->ExceptionCode==EXCEPTION_BREAKPOINT &&
       e->ExceptionRecord->ExceptionAddress==nv_rc_init_site){
        CONTEXT *c=e->ContextRecord;unsigned long long cookie=0;
        if(c->Rbp>UINTPTR_MAX-0x20 || !nv_live_read(uintptr_t(c->Rbp)+0x20,&cookie,8))
            return EXCEPTION_CONTINUE_SEARCH;
        // Verified initializer epilogue: stack-local 256-byte record at RSP+20.
        // Read that CPU stack copy, never the destination's possible BAR mapping.
        LONG slot=InterlockedIncrement(&nv_rc_init_count)-1;
        if(slot<8 && c->Rsp<=UINTPTR_MAX-0x120)
            nv_rc_init_valid[slot]=nv_live_read(uintptr_t(c->Rsp)+0x20,nv_rc_init_records[slot],256);
        // 48 8b 4d 20: MOV RCX,[RBP+20]; preserve flags and stack pointer.
        c->Rcx=cookie;c->Rip=uintptr_t(nv_rc_init_site)+4;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(!nv_live_site || e->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT ||
       e->ExceptionRecord->ExceptionAddress!=nv_live_site)return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c=e->ContextRecord;
    uintptr_t descriptor=0;
    if(c->Rcx>UINTPTR_MAX-8 || !nv_live_read(uintptr_t(c->Rcx)+8,&descriptor,8))
        return EXCEPTION_CONTINUE_SEARCH;
    if((c->Rdx&0xffff)==0x300)nv_live_observe(descriptor,unsigned(c->R8));
    // Original first instruction, 4c 8b 51 08: MOV R10,QWORD PTR [RCX+8].
    c->R10=descriptor;c->Rip=uintptr_t(nv_live_site)+4;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static bool nv_live_install(){
    HMODULE mod=GetModuleHandleW(L"nvcuvid.dll");if(!mod)return false;
    auto base=reinterpret_cast<unsigned char*>(mod);
    const IMAGE_DOS_HEADER *dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS64 *nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(nt->FileHeader.TimeDateStamp!=1781205567u || nt->OptionalHeader.SizeOfImage!=32190464u)return false;
    nv_live_image=uintptr_t(base);nv_live_image_size=nt->OptionalHeader.SizeOfImage;
    static const unsigned char expected[]={0x4c,0x8b,0x51,0x08,0x41,0x8b,0x02,0x48,
        0x8d,0x0c,0x40,0x49,0x8b,0x42,0x08,0x66,0x89,0x14,0xc8,0x41,0x8b,0x02,0x48,0x8d};
    unsigned char *site=base+0x188550;
    if(std::memcmp(site,expected,sizeof expected))return false;
    static const unsigned char rc_expected[]={0x48,0x8b,0x4d,0x20,0x48,0x33,0xcc,0xe8,0xa1,0x71,0x11,0x00};
    unsigned char *rc_site=base+0x11fba3;
    if(std::memcmp(rc_site,rc_expected,sizeof rc_expected))return false;
    nv_live_handler=AddVectoredExceptionHandler(1,nv_live_exception);
    if(!nv_live_handler)return false;
    if(!VirtualProtect(site,1,PAGE_EXECUTE_READWRITE,&nv_live_protection)){
        RemoveVectoredExceptionHandler(nv_live_handler);nv_live_handler=nullptr;return false;
    }
    nv_live_site=site;*site=0xcc;
    if(!FlushInstructionCache(GetCurrentProcess(),site,1)){nv_live_remove();return false;}
    if(!VirtualProtect(rc_site,1,PAGE_EXECUTE_READWRITE,&nv_rc_init_protection)){
        nv_live_remove();return false;
    }
    nv_rc_init_site=rc_site;*rc_site=0xcc;
    if(!FlushInstructionCache(GetCurrentProcess(),rc_site,1)){nv_live_remove();return false;}
    return true;
}
static bool nv_live_remove(){
    bool ok=true;
    if(nv_rc_init_site){
        unsigned char *site=nv_rc_init_site;*site=0x48;
        ok=FlushInstructionCache(GetCurrentProcess(),site,1)!=0;
        DWORD ignored=0;
        ok=VirtualProtect(site,1,nv_rc_init_protection,&ignored)!=0 && ok;
        nv_rc_init_site=nullptr;
    }
    if(nv_live_site){
        unsigned char *site=nv_live_site;*site=0x4c;
        ok=FlushInstructionCache(GetCurrentProcess(),site,1)!=0 && ok;
        DWORD ignored=0;
        ok=VirtualProtect(site,1,nv_live_protection,&ignored)!=0 && ok;
        nv_live_site=nullptr;
    }
    if(nv_live_handler){ok=RemoveVectoredExceptionHandler(nv_live_handler)!=0 && ok;nv_live_handler=nullptr;}
    return ok;
}
static void nv_live_save(const char *dir){
    unsigned count=nv_live_count>8?8:unsigned(nv_live_count);
    std::string json="{\"kind\":\"self-process typed commands observed immediately before EXECUTE construction; not raw GPFIFO or native Kestrel proof\",\"dropped\":"+
        std::to_string(nv_live_dropped)+",\"batches\":[";
    for(unsigned b=0;b<count;b++){
        const NvLiveBatch &batch=nv_live_batches[b];if(b)json+=",";
        json+="{\"original_count\":"+std::to_string(batch.original_count)+",\"execute_data\":"+
            std::to_string(batch.execute_data)+",\"tokens\":[";
        for(unsigned i=0;i<batch.count;i++){
            const NvLiveToken &t=batch.tokens[i];if(i)json+=",";
            json+="{\"method\":"+std::to_string(t.method)+",\"type\":"+std::to_string(t.type)+
                ",\"flags\":"+std::to_string(t.flags)+",\"data\":"+std::to_string(t.data)+
                ",\"resource_object\":\""+std::to_string(t.resource)+"\",\"va_getter_rva\":"+
                std::to_string(t.va_getter_rva)+",\"slot38_getter_rva\":"+std::to_string(t.size_getter_rva)+
                ",\"address_metadata_valid\":"+std::to_string(t.address_metadata_valid)+",\"slot38_value_not_allocation_extent\":"+
                std::to_string(t.slot38_value)+",\"gpu_va\":\""+std::to_string(t.gpu_va)+"\"}";
        }
        json+="]}";
    }
    json+="]}\n";save(dir,"live-encoder-tokens.json",json.data(),json.size());
    std::fprintf(stderr,"live typed encoder capture: batches=%u dropped=%ld\n",count,nv_live_dropped);
    std::fprintf(stderr,"RC initializer calls=%ld (at most 8 retained)\n",nv_rc_init_count);
    for(unsigned i=0;i<unsigned(nv_rc_init_count) && i<8;i++){
        if(!nv_rc_init_valid[i])continue;
        char name[64];std::snprintf(name,sizeof name,"rc-initializer-%u.bin",i);
        save(dir,name,nv_rc_init_records[i],256);
    }
}
static void nv_live_read_completed_rc(const char *dir){
    // Runs AFTER bitstream completion and outside the exception handler/CUDA
    // encoder locks. Query the current CUDA context's allocation boundary;
    // never infer a byte extent from the resource's pitch-like slot38 getter.
    if(nv_live_count!=1)return;
    const NvLiveBatch &batch=nv_live_batches[0];
    for(unsigned i=0;i<batch.count;i++){
        const NvLiveToken &t=batch.tokens[i];
        if(t.type!=4 || (t.method!=0x70c && t.method!=0x724) || !t.address_metadata_valid ||
           !t.gpu_va || t.gpu_va>UINT64_MAX-t.data)continue;
        CUdeviceptr address=CUdeviceptr(t.gpu_va+t.data),base=0;size_t extent=0;
        CUresult range=cuMemGetAddressRange(&base,&extent,address);
        std::fprintf(stderr,"post-completion RC method=%#x CUDA range status=%d\n",t.method,int(range));
        if(range!=CUDA_SUCCESS || address<base || address-base>=extent)continue;
        size_t bytes=extent-size_t(address-base);if(bytes>4096)bytes=4096;
        std::vector<unsigned char> data(bytes);
        CUresult copied=cuMemcpyDtoH(data.data(),address,bytes);
        std::fprintf(stderr,"post-completion RC method=%#x bounded bytes=%zu read status=%d\n",t.method,bytes,int(copied));
        if(copied!=CUDA_SUCCESS)continue;
        char name[64];std::snprintf(name,sizeof name,"completed-rc-%03x-prefix.bin",t.method);
        save(dir,name,data.data(),data.size());
    }
}
#endif
