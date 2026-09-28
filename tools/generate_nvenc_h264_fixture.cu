/* Windows-only independent hardware fixture. CUDA generates input; the
 * installed NVIDIA NVENC API produces the compressed frame. No CPU encoder.
 * A success here says nothing about Kestrel's native encoder submission. */
#include <cuda_runtime.h>
#include <cuda.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "third_party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h"
#include "nvenc_method_capture.h"

#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);std::exit(1);} }while(0)
#define CUDA(x) do {cudaError_t rc=(x);if(rc!=cudaSuccess){std::fprintf(stderr,"CUDA line %d: %s\n",__LINE__,cudaGetErrorString(rc));std::exit(1);}}while(0)
static NV_ENCODE_API_FUNCTION_LIST api={};
static void *encoder=nullptr;
static void enc_check(NVENCSTATUS rc,const char *call,int line){
    if(rc==NV_ENC_SUCCESS)return;
    const char *detail=encoder&&api.nvEncGetLastErrorString?api.nvEncGetLastErrorString(encoder):"";
    std::fprintf(stderr,"NVENC line %d %s: status=%d %s\n",line,call,int(rc),detail?detail:"");std::exit(1);
}
#define ENC(x) enc_check((x),#x,__LINE__)
static const unsigned W=256,H=256,PITCH=256;
__global__ void pattern(unsigned char *p,bool neutral){
    unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=W*H*3/2)return;
    if(neutral){p[i]=128;return;}
    unsigned plane=i>=W*H,offset=plane?i-W*H:i,x=offset%W,y=offset/W;
    // Smooth ramp, checkerboard and distinct U/V variation. Generate pixels
    // entirely on CUDA; the host independently verifies this documented pattern.
    if(!plane)p[i]=16+((x/2+y/3+((x/32+y/32)&1)*48)%220);
    else if(!(x&1))p[i]=48+((x/2+y*2)%160);
    else p[i]=48+((255-x/2+y*3)%160);
}
static void save(const char *dir,const char *name,const void *data,size_t bytes){
    std::string path=std::string(dir)+"/"+name;
    FILE *f=std::fopen(path.c_str(),"wb");CHECK(f);
    CHECK(std::fwrite(data,1,bytes,f)==bytes);CHECK(std::fclose(f)==0);
}
// Relative auxiliary offsets may point outside the region that held a picture
// copy. Validate every source page again; ReadProcessMemory alone does not
// exclude GPU BAR/device memory or executable pages.
static bool read_owned_data(HANDLE self,uintptr_t address,void *out,size_t bytes,size_t *got){
    *got=0;
    if(bytes>UINTPTR_MAX-address)return false;
    uintptr_t end=address+bytes;
    for(uintptr_t at=address;at<end;){
        MEMORY_BASIC_INFORMATION r={};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&r,sizeof(r)))return false;
        uintptr_t base=reinterpret_cast<uintptr_t>(r.BaseAddress);
        if(r.RegionSize>UINTPTR_MAX-base)return false;
        uintptr_t next=base+r.RegionSize;
        DWORD protection=r.Protect&0xff;
        if(next<=at || r.State!=MEM_COMMIT || (r.Protect&(PAGE_GUARD|PAGE_NOCACHE|PAGE_WRITECOMBINE)) ||
           (protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY))return false;
        at=next;
    }
    return ReadProcessMemory(self,reinterpret_cast<void*>(address),out,bytes,got) && *got==bytes;
}
#include "nvenc_live_capture.h"
// Optional, narrow reverse-engineering evidence from THIS helper only. Never
// inspect another process, executable pages, GPU BAR mappings, or dump heaps.
// A candidate must match the official CFB7 picture signature and all three
// 256x256 surface dimensions. Only that record and bounded declared controls
// are retained, not surrounding process memory.
static unsigned capture_picture_records(const char *dir,bool pictures,bool methods,unsigned qp){
    SYSTEM_INFO info={};GetSystemInfo(&info);
    uintptr_t cursor=reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    uintptr_t limit=reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    unsigned count=0,method_count=0;size_t scanned=0;
    std::vector<unsigned char> block(1024*1024+4096);
    HANDLE self=GetCurrentProcess();
    while(cursor<limit){
        MEMORY_BASIC_INFORMATION region={};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&region,sizeof(region)))break;
        uintptr_t base=reinterpret_cast<uintptr_t>(region.BaseAddress);
        if(region.RegionSize>limit-base)break;
        uintptr_t end=base+region.RegionSize;
        if(end<=cursor)break;
        DWORD protection=region.Protect&0xff;
        if(region.State==MEM_COMMIT && !(region.Protect&(PAGE_GUARD|PAGE_NOCACHE|PAGE_WRITECOMBINE)) &&
           (protection==PAGE_READWRITE || protection==PAGE_WRITECOPY)){
            for(uintptr_t start=base;start<end;){
                size_t n=(end-start<block.size())?end-start:block.size(),got=0;
                if(ReadProcessMemory(self,reinterpret_cast<void*>(start),block.data(),n,&got) && got>=0x300){
                    scanned+=got;
                    for(size_t i=0;i+8<=got;i+=4){
                        uintptr_t source=start+i,own=reinterpret_cast<uintptr_t>(block.data());
                        if(source>=own && source-own<block.size())continue;
                        unsigned char *p=block.data()+i;uint32_t magic=0;std::memcpy(&magic,p,4);
                        uint32_t next=0;std::memcpy(&next,p+4,4);
                        if(methods && method_count<32 && nvenc_capture_start(magic,next,(got-i)/4)) {
                            nvenc_capture_sequence sequence={};
                            if(nvenc_capture_decode(reinterpret_cast<const uint32_t*>(p),(got-i)/4,&sequence)) {
                                // Retain decoded methods only: no heap dump or
                                // bytes before/after the recognized sequence.
                                std::string json="{\"kind\":\"candidate host encoder methods; not hardware execution proof\",\"subchannel\":"+
                                    std::to_string(sequence.subchannel)+",\"words\":"+std::to_string(sequence.words)+
                                    ",\"application_id_observed\":"+std::to_string(sequence.application_id_observed)+",\"methods\":[";
                                for(unsigned j=0;j<sequence.count;j++){
                                    if(j)json+=",";
                                    json+="{\"method\":"+std::to_string(sequence.methods[j].method)+",\"data\":"+
                                        std::to_string(sequence.methods[j].data)+"}";
                                }
                                json+="]}\n";
                                char method_name[80];std::snprintf(method_name,sizeof(method_name),"encoder-methods-%02u.json",method_count++);
                                save(dir,method_name,json.data(),json.size());
                            }
                        }
                        if(!pictures || i+0x300>got)continue;
                        if(magic!=0xcfb70006u)continue;
                        bool match=true;
                        for(unsigned off: {4u,36u,68u}){
                            uint16_t w=0,h=0;std::memcpy(&w,p+off,2);std::memcpy(&h,p+off+2,2);
                            if(w!=255 || h!=255)match=false;
                        }
                        if(!match || count>=32)continue;
                        char name[80];std::snprintf(name,sizeof(name),"cfb7-picture-%02u.bin",count);
                        save(dir,name,p,0x300);
                        // Internal host copies have the same picture but their
                        // GPU-relative offsets do NOT refer to host controls.
                        // Retain auxiliaries only for an actual upload copy:
                        // its declared slice must contain our exact 256 MBs,
                         // requested QP, and 0..51 range, with no extra source bytes.
                        uint32_t slice_relative=0;std::memcpy(&slice_relative,p+0x18c,4);
                        unsigned char slice_check[128]={};size_t slice_got=0;
                        bool uploaded=slice_relative>=0x300 && slice_relative<=2*1024*1024 &&
                            slice_relative<=UINTPTR_MAX-(start+i) &&
                            read_owned_data(self,start+i+slice_relative,slice_check,sizeof(slice_check),&slice_got) &&
                            slice_got==sizeof(slice_check) && slice_check[0]==0 && slice_check[1]==1 &&
                             slice_check[2]==(qp<<3) && slice_check[3]==0 && slice_check[10]==51;
                        if(!uploaded){++count;continue;}
                        const unsigned fields[]={0x18c,0x190,0x194,0x198,0x1bc};
                        const unsigned sizes[]={128,192,128,192,452};
                        const char *labels[]={"slice","me","md","quant","weights"};
                        for(unsigned a=0;a<5;++a){
                            uint32_t relative=0;std::memcpy(&relative,p+fields[a],4);
                            if(relative<0x300 || relative>2*1024*1024)continue;
                            if(relative>UINTPTR_MAX-(start+i))continue;
                            uintptr_t address=start+i+relative;std::vector<unsigned char> aux(sizes[a]);size_t copied=0;
                            if(read_owned_data(self,address,aux.data(),aux.size(),&copied)){
                                std::snprintf(name,sizeof(name),"cfb7-picture-%02u-%s.bin",count,labels[a]);
                                save(dir,name,aux.data(),aux.size());
                            }
                        }
                        ++count;
                    }
                }
                if(n<=4096)break;start+=n-4096;
            }
        }
        cursor=end;
    }
    std::fprintf(stderr,"self-process CFB7 scan: readable_bytes=%zu matched_records=%u candidate_method_sequences=%u\n",scanned,count,method_count);
    return count;
}
int main(int argc,char **argv){
    CHECK(argc>=2 && argc<=7);
    bool capture_pictures=false,capture_methods=false,capture_live=false,neutral=false,mb_slice=false;
    for(int i=2;i<argc;i++){
        if(!std::strcmp(argv[i],"--capture-picture-records"))capture_pictures=true;
        else if(!std::strcmp(argv[i],"--capture-method-streams"))capture_methods=true;
        else if(!std::strcmp(argv[i],"--capture-live-methods"))capture_live=true;
        else if(!std::strcmp(argv[i],"--neutral-cqp26"))neutral=true;
        else if(!std::strcmp(argv[i],"--mb-count-slice"))mb_slice=true;
        else CHECK(false);
    }
    CUDA(cudaSetDevice(0));CUDA(cudaFree(nullptr));
    cudaDeviceProp props={};CUDA(cudaGetDeviceProperties(&props,0));
    CHECK(props.major==12); // This fixture run specifically targets Blackwell.
    CUcontext ctx=nullptr;CHECK(cuCtxGetCurrent(&ctx)==CUDA_SUCCESS && ctx);
    HMODULE dll=LoadLibraryExA("nvEncodeAPI64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(dll);
    using MaxVersion=NVENCSTATUS(NVENCAPI *)(uint32_t*);
    using Create=NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
    auto max_version=reinterpret_cast<MaxVersion>(GetProcAddress(dll,"NvEncodeAPIGetMaxSupportedVersion"));
    auto create=reinterpret_cast<Create>(GetProcAddress(dll,"NvEncodeAPICreateInstance"));CHECK(max_version&&create);
    uint32_t maximum=0;ENC(max_version(&maximum));
    CHECK(maximum>=((NVENCAPI_MAJOR_VERSION<<4)|NVENCAPI_MINOR_VERSION));
    api.version=NV_ENCODE_API_FUNCTION_LIST_VER;ENC(create(&api));
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open={};open.version=NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.apiVersion=NVENCAPI_VERSION;open.deviceType=NV_ENC_DEVICE_TYPE_CUDA;open.device=ctx;
    ENC(api.nvEncOpenEncodeSessionEx(&open,&encoder));
    NV_ENC_PRESET_CONFIG preset={};preset.version=NV_ENC_PRESET_CONFIG_VER;preset.presetCfg.version=NV_ENC_CONFIG_VER;
    ENC(api.nvEncGetEncodePresetConfigEx(encoder,NV_ENC_CODEC_H264_GUID,NV_ENC_PRESET_P1_GUID,NV_ENC_TUNING_INFO_LOW_LATENCY,&preset));
    NV_ENC_CONFIG config=preset.presetCfg;config.version=NV_ENC_CONFIG_VER;
    config.profileGUID=NV_ENC_H264_PROFILE_BASELINE_GUID;config.gopLength=1;config.frameIntervalP=1;
    config.frameFieldMode=NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    config.rcParams={};config.rcParams.version=NV_ENC_RC_PARAMS_VER;
    config.rcParams.rateControlMode=NV_ENC_PARAMS_RC_CONSTQP;
    const unsigned qp=neutral?26:18;
    config.rcParams.constQP={qp,qp,qp};config.rcParams.zeroReorderDelay=1;
    config.encodeCodecConfig={};auto &h264=config.encodeCodecConfig.h264Config;
    h264.level=NV_ENC_LEVEL_H264_31;h264.idrPeriod=1;h264.maxNumRefFrames=1;
    h264.repeatSPSPPS=1;h264.chromaFormatIDC=1;
    h264.entropyCodingMode=NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
    h264.adaptiveTransformMode=NV_ENC_H264_ADAPTIVE_TRANSFORM_DISABLE;
    h264.fmoMode=NV_ENC_H264_FMO_DISABLE;h264.bdirectMode=NV_ENC_H264_BDIRECT_MODE_DISABLE;
    // SDK mode 0 requests a macroblock count; mode 3 requests a slice count.
    // These are SDK values, NOT the firmware picture's one-bit slice_mode.
    const unsigned slice_mode=mb_slice?0u:3u,slice_data=mb_slice?(W/16)*(H/16):1u;
    h264.disableDeblockingFilterIDC=1;h264.sliceMode=slice_mode;h264.sliceModeData=slice_data;
    h264.inputBitDepth=NV_ENC_BIT_DEPTH_8;h264.outputBitDepth=NV_ENC_BIT_DEPTH_8;
    NV_ENC_INITIALIZE_PARAMS init={};init.version=NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID=NV_ENC_CODEC_H264_GUID;init.presetGUID=NV_ENC_PRESET_P1_GUID;
    init.encodeWidth=init.darWidth=init.maxEncodeWidth=W;init.encodeHeight=init.darHeight=init.maxEncodeHeight=H;
    init.frameRateNum=30;init.frameRateDen=1;init.enableEncodeAsync=0;init.enablePTD=1;
    init.reportSliceOffsets=1;init.encodeConfig=&config;init.tuningInfo=NV_ENC_TUNING_INFO_LOW_LATENCY;
    ENC(api.nvEncInitializeEncoder(encoder,&init));
    unsigned char *pixels=nullptr;CUDA(cudaMalloc(&pixels,W*H*3/2));
    pattern<<<(W*H*3/2+255)/256,256>>>(pixels,neutral);CUDA(cudaGetLastError());CUDA(cudaDeviceSynchronize());
    std::vector<unsigned char> input(W*H*3/2);CUDA(cudaMemcpy(input.data(),pixels,input.size(),cudaMemcpyDeviceToHost));
    save(argv[1],"input.nv12",input.data(),input.size());
    NV_ENC_REGISTER_RESOURCE resource={};resource.version=NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType=NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;resource.width=W;resource.height=H;
    resource.pitch=PITCH;resource.resourceToRegister=pixels;resource.bufferFormat=NV_ENC_BUFFER_FORMAT_NV12;
    resource.bufferUsage=NV_ENC_INPUT_IMAGE;ENC(api.nvEncRegisterResource(encoder,&resource));
    NV_ENC_MAP_INPUT_RESOURCE map={};map.version=NV_ENC_MAP_INPUT_RESOURCE_VER;map.registeredResource=resource.registeredResource;
    ENC(api.nvEncMapInputResource(encoder,&map));CHECK(map.mappedBufferFmt==NV_ENC_BUFFER_FORMAT_NV12);
    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream={};bitstream.version=NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    ENC(api.nvEncCreateBitstreamBuffer(encoder,&bitstream));
    NV_ENC_PIC_PARAMS pic={};pic.version=NV_ENC_PIC_PARAMS_VER;pic.inputWidth=W;pic.inputHeight=H;pic.inputPitch=PITCH;
    pic.inputBuffer=map.mappedResource;pic.outputBitstream=bitstream.bitstreamBuffer;
    pic.bufferFmt=NV_ENC_BUFFER_FORMAT_NV12;pic.pictureStruct=NV_ENC_PIC_STRUCT_FRAME;
    pic.encodePicFlags=NV_ENC_PIC_FLAG_FORCEIDR|NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    pic.codecPicParams.h264PicParams.sliceMode=slice_mode;pic.codecPicParams.h264PicParams.sliceModeData=slice_data;
    if(capture_live)CHECK(nv_live_install());
    ENC(api.nvEncEncodePicture(encoder,&pic));
    NV_ENC_LOCK_BITSTREAM lock={};lock.version=NV_ENC_LOCK_BITSTREAM_VER;lock.outputBitstream=bitstream.bitstreamBuffer;
    uint32_t offsets[W/16*H/16]={};lock.sliceOffsets=offsets;lock.doNotWait=0;
    ENC(api.nvEncLockBitstream(encoder,&lock));
    if(capture_live){CHECK(nv_live_remove());nv_live_save(argv[1]);nv_live_read_completed_rc(argv[1]);}
    CHECK(lock.bitstreamBufferPtr && lock.bitstreamSizeInBytes>0);
    // Preserve raw driver output before validation; no NAL stripping/rewriting.
    save(argv[1],"frame.h264",lock.bitstreamBufferPtr,lock.bitstreamSizeInBytes);
    if(capture_pictures || capture_methods)capture_picture_records(argv[1],capture_pictures,capture_methods,qp);
    std::printf("{\"device\":\"%s\",\"compute_major\":%d,\"compute_minor\":%d,\"max_api_version\":%u,\"bytes\":%u,\"slices\":%u,\"hw_status\":%u,\"picture_type\":%u,\"avg_qp\":%u}\n",
                props.name,props.major,props.minor,maximum,lock.bitstreamSizeInBytes,lock.numSlices,lock.hwEncodeStatus,unsigned(lock.pictureType),lock.frameAvgQP);
    // NVIDIA NvEncoder::GetEncodedPacket gates on the API return value, not
    // hwEncodeStatus (whose integer values are not specified in this header).
    // Retain that raw field above; do not invent a zero-success interpretation.
    // https://github.com/NVIDIA/video-sdk-samples/blob/master/Samples/NvCodec/NvEncoder/NvEncoder.cpp
    CHECK(lock.numSlices==1 && lock.pictureType==NV_ENC_PIC_TYPE_IDR);
    ENC(api.nvEncUnlockBitstream(encoder,bitstream.bitstreamBuffer));
    NV_ENC_PIC_PARAMS eos={};eos.version=NV_ENC_PIC_PARAMS_VER;eos.encodePicFlags=NV_ENC_PIC_FLAG_EOS;
    ENC(api.nvEncEncodePicture(encoder,&eos));
    ENC(api.nvEncUnmapInputResource(encoder,map.mappedResource));
    ENC(api.nvEncUnregisterResource(encoder,resource.registeredResource));
    ENC(api.nvEncDestroyBitstreamBuffer(encoder,bitstream.bitstreamBuffer));
    ENC(api.nvEncDestroyEncoder(encoder));encoder=nullptr;CUDA(cudaFree(pixels));CHECK(FreeLibrary(dll));
    return 0;
}
