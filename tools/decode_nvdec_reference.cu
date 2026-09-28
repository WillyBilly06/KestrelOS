/* Exact Windows NVDEC oracle, not native Kestrel proof. The installed NVIDIA
 * parser supplies CUVID picture state to the dedicated video decoder. CUDA
 * copies its mapped output; no software entropy decoder, scaling or filtering.
 * API contracts: pinned NVIDIA dynlink_cuviddec.h/dynlink_nvcuvid.h. */
#include <windows.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include "third_party/nv-codec-headers/include/ffnvcodec/dynlink_nvcuvid.h"

#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Event {const char *name;int result;};
static std::vector<Event> events;
static void checked(CUresult rc,const char *name,int line){
    events.push_back({name,int(rc)});
    if(rc!=CUDA_SUCCESS){const char *error=nullptr;cuGetErrorString(rc,&error);
        std::fprintf(stderr,"CUVID/CUDA line %d %s: status=%d %s\n",line,name,int(rc),error?error:"");std::exit(1);}
}
#define API(x) checked((x),#x,__LINE__)
#define LOAD(name) static t##name *name=nullptr
LOAD(cuvidGetDecoderCaps);LOAD(cuvidCreateDecoder);LOAD(cuvidDestroyDecoder);
LOAD(cuvidDecodePicture);LOAD(cuvidGetDecodeStatus);LOAD(cuvidMapVideoFrame64);LOAD(cuvidUnmapVideoFrame64);
LOAD(cuvidCreateVideoParser);LOAD(cuvidParseVideoData);LOAD(cuvidDestroyVideoParser);
#undef LOAD
struct Decode {
    CUvideodecoder decoder=nullptr;
    unsigned sequences=0,pictures=0,displays=0,width=0,height=0,pitch=0,surfaces=0;
    int picture_index=-1,status=0;
    CUVIDDECODECAPS caps={};
    std::vector<unsigned char> nv12;
    CUVIDPICPARAMS captured={};
    std::vector<unsigned char> parser_bitstream;
    std::vector<unsigned> slice_offsets;
};
static int CUDAAPI sequence(void *user,CUVIDEOFORMAT *format){
    Decode &d=*static_cast<Decode *>(user);CHECK(++d.sequences==1 && !d.decoder && format);
    CHECK(format->codec==cudaVideoCodec_H264 && format->progressive_sequence);
    CHECK(format->chroma_format==cudaVideoChromaFormat_420 && !format->bit_depth_luma_minus8 && !format->bit_depth_chroma_minus8);
    CHECK(format->coded_width==256 && format->coded_height==256);
    CHECK(!format->display_area.left && !format->display_area.top && format->display_area.right==256 && format->display_area.bottom==256);
    d.width=format->coded_width;d.height=format->coded_height;d.surfaces=format->min_num_decode_surfaces;
    CHECK(d.surfaces>=1 && d.surfaces<=64);
    d.caps.eCodecType=format->codec;d.caps.eChromaFormat=format->chroma_format;
    API(cuvidGetDecoderCaps(&d.caps));CHECK(d.caps.bIsSupported);
    CHECK(d.caps.nOutputFormatMask&(1u<<cudaVideoSurfaceFormat_NV12));
    CHECK(d.width>=d.caps.nMinWidth && d.height>=d.caps.nMinHeight && d.width<=d.caps.nMaxWidth && d.height<=d.caps.nMaxHeight);
    CHECK(d.width*d.height/256<=d.caps.nMaxMBCount);
    CUVIDDECODECREATEINFO config={};config.CodecType=format->codec;config.ChromaFormat=format->chroma_format;
    config.ulCreationFlags=cudaVideoCreate_PreferCUVID; // explicitly dedicated video engines, not PreferCUDA
    config.ulWidth=config.ulMaxWidth=config.ulTargetWidth=d.width;
    config.ulHeight=config.ulMaxHeight=config.ulTargetHeight=d.height;
    config.ulNumDecodeSurfaces=d.surfaces;config.ulNumOutputSurfaces=1;config.ulIntraDecodeOnly=1;
    config.display_area.right=short(d.width);config.display_area.bottom=short(d.height);
    config.target_rect.right=short(d.width);config.target_rect.bottom=short(d.height);
    config.OutputFormat=cudaVideoSurfaceFormat_NV12;config.DeinterlaceMode=cudaVideoDeinterlaceMode_Weave;
    API(cuvidCreateDecoder(&d.decoder,&config));return int(d.surfaces);
}
static int CUDAAPI picture(void *user,CUVIDPICPARAMS *pic){
    Decode &d=*static_cast<Decode *>(user);CHECK(d.decoder && pic && ++d.pictures==1);
    CHECK(pic->PicWidthInMbs==16 && pic->FrameHeightInMbs==16 && !pic->field_pic_flag && !pic->second_field);
    CHECK(pic->intra_pic_flag && pic->ref_pic_flag && pic->nNumSlices==1 && pic->nBitstreamDataLen && pic->pBitstreamData);
    d.captured=*pic;
    d.parser_bitstream.assign(pic->pBitstreamData,pic->pBitstreamData+pic->nBitstreamDataLen);
    d.slice_offsets.assign(pic->pSliceDataOffsets,pic->pSliceDataOffsets+pic->nNumSlices);
    d.picture_index=pic->CurrPicIdx;CHECK(d.picture_index>=0 && unsigned(d.picture_index)<d.surfaces);
    API(cuvidDecodePicture(d.decoder,pic));return 1;
}
static int CUDAAPI display(void *user,CUVIDPARSERDISPINFO *info){
    Decode &d=*static_cast<Decode *>(user);CHECK(d.decoder && info && ++d.displays==1 && d.pictures==1);
    CHECK(info->picture_index==d.picture_index && info->progressive_frame && !info->repeat_first_field);
    CUVIDPROCPARAMS proc={};proc.progressive_frame=1;proc.top_field_first=info->top_field_first;
    unsigned long long device=0;API(cuvidMapVideoFrame64(d.decoder,info->picture_index,&device,&d.pitch,&proc));
    CHECK(device && d.pitch>=d.width && d.pitch<=65536);
    CUVIDGETDECODESTATUS status={};API(cuvidGetDecodeStatus(d.decoder,info->picture_index,&status));
    d.status=int(status.decodeStatus);CHECK(status.decodeStatus==cuvidDecodeStatus_Success);
    d.nv12.resize(d.width*d.height*3/2,0xa5);
    // Preserve coded dimensions and NV12 UV interleaving. The mapped API surface
    // is pitch-linear; its padding is not part of the comparison oracle.
    CUDA_MEMCPY2D y={};y.srcMemoryType=CU_MEMORYTYPE_DEVICE;y.srcDevice=device;y.srcPitch=d.pitch;
    y.dstMemoryType=CU_MEMORYTYPE_HOST;y.dstHost=d.nv12.data();y.dstPitch=d.width;y.WidthInBytes=d.width;y.Height=d.height;
    API(cuMemcpy2D(&y));
    CUDA_MEMCPY2D uv=y;uv.srcDevice=device+(unsigned long long)d.pitch*d.height;
    uv.dstHost=d.nv12.data()+d.width*d.height;uv.Height=d.height/2;API(cuMemcpy2D(&uv));
    API(cuCtxSynchronize());API(cuvidUnmapVideoFrame64(d.decoder,device));return 1;
}
static void save(const char *path,const std::vector<unsigned char> &bytes){
    FILE *out=std::fopen(path,"wb");CHECK(out);CHECK(std::fwrite(bytes.data(),1,bytes.size(),out)==bytes.size());CHECK(!std::fclose(out));
}
static void print_picture(const Decode &d){
    const CUVIDPICPARAMS &p=d.captured;const CUVIDH264PICPARAMS &h=p.CodecSpecific.h264;
    std::printf(",\"cuvid_picture\":{");
#define PF(name) std::printf("\"" #name "\":%d,",int(p.name))
    PF(PicWidthInMbs);PF(FrameHeightInMbs);PF(CurrPicIdx);PF(field_pic_flag);PF(bottom_field_flag);PF(second_field);
    PF(nBitstreamDataLen);PF(nNumSlices);PF(ref_pic_flag);PF(intra_pic_flag);
#undef PF
#define HF(name) std::printf("\"" #name "\":%d,",int(h.name))
    HF(log2_max_frame_num_minus4);HF(pic_order_cnt_type);HF(log2_max_pic_order_cnt_lsb_minus4);
    HF(delta_pic_order_always_zero_flag);HF(frame_mbs_only_flag);HF(direct_8x8_inference_flag);HF(num_ref_frames);
    HF(residual_colour_transform_flag);HF(bit_depth_luma_minus8);HF(bit_depth_chroma_minus8);HF(qpprime_y_zero_transform_bypass_flag);
    HF(entropy_coding_mode_flag);HF(pic_order_present_flag);HF(num_ref_idx_l0_active_minus1);HF(num_ref_idx_l1_active_minus1);
    HF(weighted_pred_flag);HF(weighted_bipred_idc);HF(pic_init_qp_minus26);HF(deblocking_filter_control_present_flag);
    HF(redundant_pic_cnt_present_flag);HF(transform_8x8_mode_flag);HF(MbaffFrameFlag);HF(constrained_intra_pred_flag);
    HF(chroma_qp_index_offset);HF(second_chroma_qp_index_offset);HF(frame_num);HF(fmo_aso_enable);
    HF(num_slice_groups_minus1);HF(slice_group_map_type);HF(pic_init_qs_minus26);HF(slice_group_change_rate_minus1);
    HF(mb_adaptive_frame_field_flag);
#undef HF
    std::printf("\"h264_ref_pic_flag\":%d,\"CurrFieldOrderCnt\":[%d,%d],\"slice_offsets\":[",h.ref_pic_flag,h.CurrFieldOrderCnt[0],h.CurrFieldOrderCnt[1]);
    for(size_t i=0;i<d.slice_offsets.size();i++)std::printf("%s%u",i?",":"",d.slice_offsets[i]);
    std::printf("],\"dpb\":[");
    for(unsigned i=0;i<16;i++){const auto &r=h.dpb[i];
        std::printf("%s{\"PicIdx\":%d,\"FrameIdx\":%d,\"is_long_term\":%d,\"not_existing\":%d,\"used_for_reference\":%d,\"FieldOrderCnt\":[%d,%d]}",
            i?",":"",r.PicIdx,r.FrameIdx,r.is_long_term,r.not_existing,r.used_for_reference,r.FieldOrderCnt[0],r.FieldOrderCnt[1]);}
    std::printf("],\"WeightScale4x4\":[");
    for(unsigned i=0;i<96;i++)std::printf("%s%u",i?",":"",unsigned(h.WeightScale4x4[i/16][i%16]));
    std::printf("],\"WeightScale8x8\":[");
    for(unsigned i=0;i<128;i++)std::printf("%s%u",i?",":"",unsigned(h.WeightScale8x8[i/64][i%64]));
    std::printf("]}");
}
int main(int argc,char **argv){
    CHECK(argc==4);FILE *input=std::fopen(argv[1],"rb");CHECK(input);
    CHECK(!std::fseek(input,0,SEEK_END));long bytes=std::ftell(input);CHECK(bytes>0 && bytes<=16*1024*1024);
    CHECK(!std::fseek(input,0,SEEK_SET));std::vector<unsigned char> stream(size_t(bytes),0);
    CHECK(std::fread(stream.data(),1,stream.size(),input)==stream.size());CHECK(!std::fclose(input));
    API(cuInit(0));CUdevice device;API(cuDeviceGet(&device,0));
    int major=0,minor=0,driver=0;API(cuDeviceGetAttribute(&major,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,device));
    API(cuDeviceGetAttribute(&minor,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,device));CHECK(major==12);
    API(cuDriverGetVersion(&driver));char name[256]={};API(cuDeviceGetName(name,sizeof name,device));
    CUcontext context=nullptr;API(cuDevicePrimaryCtxRetain(&context,device));API(cuCtxSetCurrent(context));
    HMODULE library=LoadLibraryExA("nvcuvid.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(library);
#define BIND(name) name=reinterpret_cast<t##name *>(GetProcAddress(library,#name));CHECK(name)
    BIND(cuvidGetDecoderCaps);BIND(cuvidCreateDecoder);BIND(cuvidDestroyDecoder);BIND(cuvidDecodePicture);
    BIND(cuvidGetDecodeStatus);BIND(cuvidMapVideoFrame64);BIND(cuvidUnmapVideoFrame64);
    BIND(cuvidCreateVideoParser);BIND(cuvidParseVideoData);BIND(cuvidDestroyVideoParser);
#undef BIND
    Decode decoded;CUvideoparser parser=nullptr;CUVIDPARSERPARAMS params={};
    params.CodecType=cudaVideoCodec_H264;params.ulMaxNumDecodeSurfaces=1;params.ulMaxDisplayDelay=0;
    params.ulErrorThreshold=0;params.pUserData=&decoded;params.pfnSequenceCallback=sequence;
    params.pfnDecodePicture=picture;params.pfnDisplayPicture=display;API(cuvidCreateVideoParser(&parser,&params));
    CUVIDSOURCEDATAPACKET packet={};packet.payload=stream.data();packet.payload_size=static_cast<unsigned long>(stream.size());
    API(cuvidParseVideoData(parser,&packet));
    CUVIDSOURCEDATAPACKET eos={};eos.flags=CUVID_PKT_ENDOFSTREAM;API(cuvidParseVideoData(parser,&eos));
    CHECK(decoded.sequences==1 && decoded.pictures==1 && decoded.displays==1 && decoded.nv12.size()==98304);
    API(cuvidDestroyVideoParser(parser));API(cuvidDestroyDecoder(decoded.decoder));
    API(cuCtxSetCurrent(nullptr));API(cuDevicePrimaryCtxRelease(device));CHECK(FreeLibrary(library));
    save(argv[2],decoded.nv12);save(argv[3],decoded.parser_bitstream);
    std::printf("{\"device\":\"%s\",\"compute_major\":%d,\"compute_minor\":%d,\"cuda_driver_version\":%d,",name,major,minor,driver);
    std::printf("\"coded_width\":%u,\"coded_height\":%u,\"mapped_pitch\":%u,\"output_pitch\":%u,\"output_bytes\":%zu,",decoded.width,decoded.height,decoded.pitch,decoded.width,decoded.nv12.size());
    std::printf("\"decode_surfaces\":%u,\"nvdec_engines\":%u,\"decode_status\":%d,\"sequences\":%u,\"pictures\":%u,\"displays\":%u,\"api_calls\":[",decoded.surfaces,unsigned(decoded.caps.nNumNVDECs),decoded.status,decoded.sequences,decoded.pictures,decoded.displays);
    for(size_t i=0;i<events.size();i++)std::printf("%s{\"call\":\"%s\",\"status\":%d}",i?",":"",events[i].name,events[i].result);
    std::printf("]");print_picture(decoded);std::printf("}\n");return 0;
}
