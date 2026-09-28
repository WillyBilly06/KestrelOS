/* Checked 8-bit NV12 CPU data transport for Blackwell video surfaces.
 *
 * Evidence: out/mesa-ref/src/nouveau/nil/tiling.rs::GOBType::choose chooses
 * Blackwell8Bit for the R8 Y plane and Blackwell16Bit for R8G8 UV. Video usage
 * forces y_log2=1: two 64x8-byte GOBs vertically, i.e. 64x16-byte blocks.
 * copy.rs::CopyGOBBlackwell2D1BPP/2BPP supplies the within-GOB byte order;
 * BlockPointer supplies row-major block addressing. These are NOT the common
 * Tegra/Fermi swizzles. Y and interleaved UV have DIFFERENT byte orders.
 *
 * This implements data movement only, not NVDEC/NVENC execution. It requires
 * coherent caller-owned CPU mappings, not raw PRAMIN or opaque GPU addresses.
 * No cropping: convert the complete coded image; crop using separate metadata.
 */
#ifndef KESTREL_NV_VIDEO_LAYOUT_H
#define KESTREL_NV_VIDEO_LAYOUT_H

#define NV_VIDEO_LINEAR_NV12 0u
#define NV_VIDEO_BLACKWELL_GOB2_NV12 1u
typedef unsigned long long nv_video_size;
typedef __UINTPTR_TYPE__ nv_video_address;

typedef struct {
    unsigned int format, width, height, y_pitch, uv_pitch, y_rows, uv_rows;
    nv_video_size y_bytes, uv_bytes, uv_offset, bytes;
} nv_video_nv12_layout_s;

/* Zero pitch/row arguments select the minimum required padding. Explicit
 * pitches/heights are accepted only if sufficient and properly aligned.
 * Linear rows need no alignment; Blackwell pitches are multiples of 64 bytes,
 * and each plane's allocated height is a multiple of 16 rows. NV12 has even,
 * nonzero coded width/height; UV row WIDTH is bytes, not two-byte texel count.
 * Layouts use contiguous planes, UV immediately following the padded Y plane.
 * On failure *out is zero. Returns 1 for success, 0 for invalid/overflow.
 */
static inline int nv_video_nv12_layout_init(nv_video_nv12_layout_s *out,
    unsigned int format, unsigned int width, unsigned int height,
    unsigned int y_pitch, unsigned int uv_pitch,
    unsigned int y_rows, unsigned int uv_rows) {
    if(!out)return 0;
    *out=(nv_video_nv12_layout_s){0};
    if(format>NV_VIDEO_BLACKWELL_GOB2_NV12 || !width || !height || ((width|height)&1u))return 0;
    unsigned int x_align=format==NV_VIDEO_LINEAR_NV12?1u:64u;
    unsigned int y_align=format==NV_VIDEO_LINEAR_NV12?1u:16u;
    nv_video_size min_pitch=((nv_video_size)width+x_align-1u)&~(nv_video_size)(x_align-1u);
    nv_video_size min_y=((nv_video_size)height+y_align-1u)&~(nv_video_size)(y_align-1u);
    nv_video_size min_uv=((nv_video_size)(height/2u)+y_align-1u)&~(nv_video_size)(y_align-1u);
    if(min_pitch>0xffffffffu||min_y>0xffffffffu||min_uv>0xffffffffu)return 0;
    if(!y_pitch)y_pitch=(unsigned int)min_pitch;
    if(!uv_pitch)uv_pitch=(unsigned int)min_pitch;
    if(!y_rows)y_rows=(unsigned int)min_y;
    if(!uv_rows)uv_rows=(unsigned int)min_uv;
    if(y_pitch<min_pitch||uv_pitch<min_pitch||y_rows<min_y||uv_rows<min_uv||
       (y_pitch&(x_align-1u))||(uv_pitch&(x_align-1u))||
       (y_rows&(y_align-1u))||(uv_rows&(y_align-1u)))return 0;
    nv_video_size y_bytes=(nv_video_size)y_pitch*y_rows;
    nv_video_size uv_bytes=(nv_video_size)uv_pitch*uv_rows;
    if(uv_bytes>~(nv_video_size)0-y_bytes)return 0;
    *out=(nv_video_nv12_layout_s){.format=format,.width=width,.height=height,
        .y_pitch=y_pitch,.uv_pitch=uv_pitch,.y_rows=y_rows,.uv_rows=uv_rows,
        .y_bytes=y_bytes,.uv_bytes=uv_bytes,.uv_offset=y_bytes,.bytes=y_bytes+uv_bytes};
    return 1;
}

/* Revalidate externally supplied or modified layout descriptions. Never trust
 * cached byte counts supplied by a caller instead of recalculating geometry. */
static inline int nv_video_nv12_layout_valid(const nv_video_nv12_layout_s *l){
    nv_video_nv12_layout_s expected;
    return l && nv_video_nv12_layout_init(&expected,l->format,l->width,l->height,
        l->y_pitch,l->uv_pitch,l->y_rows,l->uv_rows) &&
        l->y_pitch==expected.y_pitch && l->uv_pitch==expected.uv_pitch &&
        l->y_rows==expected.y_rows && l->uv_rows==expected.uv_rows &&
        l->y_bytes==expected.y_bytes && l->uv_bytes==expected.uv_bytes &&
        l->uv_offset==expected.uv_offset && l->bytes==expected.bytes;
}

/* Call only with previously validated pitch/coordinates/plane. */
static inline nv_video_size nv_video_gob2_offset_unchecked(unsigned int pitch,
    unsigned int x, unsigned int y, unsigned int plane){
    nv_video_size block=(nv_video_size)(y/16u)*pitch*16u+(nv_video_size)(x/64u)*1024u;
    unsigned int xx=x%64u,yy=y%8u;
    unsigned int in_gob=plane==0?
        (xx/8u)*64u+yy*8u+xx%8u:
        (xx/32u)*256u+((xx%32u)/16u)*128u+(yy/4u)*64u+(yy%4u)*16u+xx%16u;
    return block+((y%16u)/8u)*512u+in_gob;
}

/* Checked byte offset in the complete two-plane allocation. Coordinates are
 * in BYTE columns and plane rows, including allocated padding. For logical
 * UV texel (u,v), the U/V bytes are x=2*u and 2*u+1, y=v. */
static inline int nv_video_nv12_offset(const nv_video_nv12_layout_s *l,
    unsigned int plane,unsigned int x,unsigned int y,nv_video_size *out){
    if(!out)return 0;
    *out=0;
    if(!nv_video_nv12_layout_valid(l)||plane>1)return 0;
    unsigned int pitch=plane?l->uv_pitch:l->y_pitch,rows=plane?l->uv_rows:l->y_rows;
    if(x>=pitch||y>=rows)return 0;
    nv_video_size offset=l->format==NV_VIDEO_LINEAR_NV12?(nv_video_size)y*pitch+x:
        nv_video_gob2_offset_unchecked(pitch,x,y,plane);
    *out=(plane?l->uv_offset:0)+offset;return 1;
}

/* Reject pointer-range wrapping before dereferencing either image. The caller
 * still owns responsibility for the actual mapped allocation's accessibility.
 * Requiring PTRDIFF_MAX avoids unrepresentable C object/pointer arithmetic. */
static inline int nv_video_cpu_span(const void *p,nv_video_size bytes){
    nv_video_address a=(nv_video_address)p;
    return p && bytes && bytes<=(nv_video_size)__PTRDIFF_MAX__ &&
        bytes<=(nv_video_size)(~(nv_video_address)0-a);
}

/* Bidirectional LINEAR_NV12 <-> BLACKWELL_GOB2_NV12. Both layouts must describe
 * the same complete coded image. All validation, capacity and alias checks
 * happen BEFORE any destination write. In-place/overlapping conversions are
 * unsupported. Destination pitch/height padding is deterministically zeroed;
 * source padding is ignored. Supplied capacities may exceed required sizes;
 * bytes outside the destination layout's extent are never modified.
 * Returns 1 on success, 0 with destination unchanged on invalid arguments.
 */
static inline int nv_video_nv12_convert(void *dst,nv_video_size dst_capacity,
    const nv_video_nv12_layout_s *dst_layout,const void *src,nv_video_size src_capacity,
    const nv_video_nv12_layout_s *src_layout){
    if(!nv_video_nv12_layout_valid(dst_layout)||!nv_video_nv12_layout_valid(src_layout))return 0;
    /* Snapshot descriptors before writing in case caller metadata is adjacent
     * to the image allocation; don't re-read mutable state inside pixel loops. */
    nv_video_nv12_layout_s d=*dst_layout,s=*src_layout;
    if(d.format==s.format||d.width!=s.width||d.height!=s.height||
       dst_capacity<d.bytes||src_capacity<s.bytes||!nv_video_cpu_span(dst,d.bytes)||
       !nv_video_cpu_span(src,s.bytes))return 0;
    nv_video_address da=(nv_video_address)dst,sa=(nv_video_address)src;
    if(da<sa+s.bytes && sa<da+d.bytes)return 0;
    unsigned char *to=(unsigned char *)dst;const unsigned char *from=(const unsigned char *)src;
    for(nv_video_size i=0;i<d.bytes;i++)to[i]=0;
    for(unsigned int plane=0;plane<2;plane++){
        unsigned int rows=plane?d.height/2u:d.height;
        unsigned int dp=plane?d.uv_pitch:d.y_pitch,sp=plane?s.uv_pitch:s.y_pitch;
        nv_video_size db=plane?d.uv_offset:0,sb=plane?s.uv_offset:0;
        for(unsigned int y=0;y<rows;y++){
            for(unsigned int x=0;x<d.width;x++){
                nv_video_size di=d.format==NV_VIDEO_LINEAR_NV12?(nv_video_size)y*dp+x:nv_video_gob2_offset_unchecked(dp,x,y,plane);
                nv_video_size si=s.format==NV_VIDEO_LINEAR_NV12?(nv_video_size)y*sp+x:nv_video_gob2_offset_unchecked(sp,x,y,plane);
                to[db+di]=from[sb+si];
            }
        }
    }
    return 1;
}
#endif
