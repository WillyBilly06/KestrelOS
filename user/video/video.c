/* Application-facing baseline H.264 IDR inspection/native codec utility. */
#include "kestrel.h"

static int video_call(kvideo_request_t *r) {
    for (unsigned i=0;i<100;i++) {
        int rc=gpu_video(r);
        if (!rc || errno!=EBUSY) return rc;
        sleep_ms(10);
    }
    errno=EBUSY;
    return -1;
}

static unsigned dimension(const char *s) {
    unsigned value=0;
    if (!s || !*s) return 0;
    for (;*s;s++) {
        if (*s<'0' || *s>'9' || value>4096u/10u) return 0;
        value=value*10u+(unsigned)(*s-'0');
        if (value>4096u) return 0;
    }
    return value;
}

int main(int argc,char **argv) {
    bool decode=argc>=2 && !strcmp(argv[1],"decode");
    bool encode=argc>=2 && !strcmp(argv[1],"encode");
    if ((decode && argc!=4) || (encode && argc!=6) ||
        (!decode && !encode && (argc!=3 || strcmp(argv[1],"info")))) {
        printf("usage: video info input.h264\n"
               "       video decode input.h264 output.nv12\n"
               "       video encode input.nv12 output.h264 width height\n"
               "One baseline progressive 8-bit H.264 IDR with SPS/PPS only.\n"
               "Decode uses NVDEC; no software fallback. Output is coded NV12, not a container.\n"
               "Experimental NVENC encode: 256x256 packed NV12, one lossy IDR at QP 26.\n"
               "Native encoding remains unverified; this is not a general video encoder.\n");
        return 1;
    }
    if ((decode || encode) && !strcmp(argv[2],argv[3])) {
        printf("video: input and output paths must differ\n"); return 1;
    }
    unsigned width=encode?dimension(argv[4]):0,height=encode?dimension(argv[5]):0;
    if (encode && (width!=KVIDEO_ENCODE_WIDTH || height!=KVIDEO_ENCODE_HEIGHT)) {
        printf("video: native encoder currently accepts %ux%u; no implicit resizing\n",
               KVIDEO_ENCODE_WIDTH,KVIDEO_ENCODE_HEIGHT); return 1;
    }
    int fd=open(argv[2],O_RDONLY);
    if (fd<0) { printf("video: cannot open input (errno=%d)\n",errno); return 1; }
    off_t size=lseek(fd,0,SEEK_END);
    if (size<=0 || (uint64_t)size>KVIDEO_INPUT_MAX || lseek(fd,0,SEEK_SET)<0) {
        printf("video: input must be a seekable file of 1..%u bytes\n",KVIDEO_INPUT_MAX);
        close(fd); return 1;
    }
    if (encode && (uint64_t)size!=KVIDEO_ENCODE_INPUT_BYTES) {
        printf("video: input must contain exactly %u packed NV12 bytes\n",KVIDEO_ENCODE_INPUT_BYTES);
        close(fd); return 1;
    }
    unsigned char *input=malloc((size_t)size),*output=NULL;
    if (!input) { close(fd); printf("video: out of memory\n"); return 1; }
    size_t at=0;
    while (at<(size_t)size) {
        ssize_t n=read(fd,input+at,(size_t)size-at);
        if (n<=0 || (size_t)n>(size_t)size-at) break;
        at+=(size_t)n;
    }
    close(fd);
    if (at!=(size_t)size) { free(input); printf("video: incomplete input read\n"); return 1; }
    kvideo_request_t r={.version=KVIDEO_ABI,
        .operation=encode?KVIDEO_H264_ENCODE_PLAN:KVIDEO_H264_INSPECT,
        .input=(uintptr_t)input,.input_bytes=(uint64_t)size,
        .coded_width=width,.coded_height=height};
    int result=1;
    if (video_call(&r)) goto failed;
    if (!encode) printf("H.264 IDR: coded %ux%u, display %ux%u, crop %u/%u/%u/%u, NV12 %u bytes\n",
        r.coded_width,r.coded_height,r.display_width,r.display_height,
        r.crop_left,r.crop_top,r.crop_right,r.crop_bottom,r.required_bytes);
    if (!decode && !encode) { printf("Headers inspected only; GPU decoding not attempted.\n"); result=0; goto done; }
    if (!r.required_bytes || r.required_bytes>KVIDEO_OUTPUT_MAX) { errno=EIO; goto failed; }
    output=malloc(r.required_bytes);
    if (!output) { errno=ENOMEM; goto failed; }
    r.operation=encode?KVIDEO_H264_ENCODE_IDR:KVIDEO_H264_DECODE_IDR;
    r.output=(uintptr_t)output;r.output_capacity=r.required_bytes;
    if (video_call(&r)) goto failed;
    if (r.phase!=KVIDEO_PHASE_COMPLETE || !r.written_bytes ||
        r.written_bytes>r.required_bytes || (!encode && r.written_bytes!=r.required_bytes)) {
        errno=EIO; goto failed;
    }
    /* Open/truncate output only after a complete hardware frame is available. */
    if (write_file(argv[3],output,r.written_bytes)<0) {
        printf("video: output write failed (errno=%d); output file may be partial\n",errno);
        goto done;
    }
    if (encode) printf("NVENC returned %u H.264 bytes to %s (single QP-26 IDR; decode not checked).\n",
                       r.written_bytes,argv[3]);
    else printf("NVDEC returned %u macroblocks, %u NV12 bytes to %s (pitch %u).\n",
           r.decoded_mbs,r.written_bytes,argv[3],r.pitch);
    result=0;goto done;
failed:
    printf("video: failed errno=%d phase=%u headers=%u firmware=%#x slice=%#x MB=%u error-MB=%u\n",
        errno,r.phase,r.parse_status,r.firmware_error,r.slice_error,r.decoded_mbs,r.error_mbs);
done:
    free(output);free(input);return result;
}
