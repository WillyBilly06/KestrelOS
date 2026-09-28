/* Ordered widget painting: solid, gradient, image, fade and glyph-mask commands.
 * One thread owns each destination pixel for the whole batch, so overlapping
 * commands obey painter's order without atomics or inter-launch barriers.
 * Source storage must not alias the destination and remains immutable until
 * completion. Kernel-side surface ownership/bounds validation is still required.
 * Launch as 16x16 threads, over clip_width x clip_height, using a QMD with
 * explicit dimensions. The existing triangle shader is not modified.
 */
#include "../include/kestrel/gpu2d.h"

static __device__ __forceinline__ unsigned int widget_mix(
    unsigned int a, unsigned int b, unsigned int alpha)
{
    /* Match libgui colour_mix(), including its endpoint/upper-byte semantics. */
    if (!alpha) return a;
    if (alpha >= 255u) return b;
    unsigned int inv = 255u - alpha, result = 0;
    for (unsigned int shift = 0; shift < 24; shift += 8)
        result |= ((((a >> shift) & 255u) * inv +
                    ((b >> shift) & 255u) * alpha) / 255u) << shift;
    return result;
}

static __device__ __forceinline__ unsigned int icon_load(const unsigned char *s)
{
    return (unsigned)s[0] | ((unsigned)s[1]<<8) | ((unsigned)s[2]<<16) | ((unsigned)s[3]<<24);
}
static __device__ __forceinline__ unsigned int icon_lerp(unsigned a, unsigned b, unsigned t)
{
    unsigned result=0;
    for(unsigned shift=0;shift<32;shift+=8)
        result |= ((((a>>shift)&255u)*(256u-t)+((b>>shift)&255u)*t)>>8)<<shift;
    return result;
}

static __device__ __forceinline__ unsigned rounded_coverage(unsigned dx,unsigned dy,unsigned radius)
{
    unsigned distance=dx*dx+dy*dy;
    unsigned inner=radius?(radius-1u)*(radius-1u):1u,outer=(radius+1u)*(radius+1u);
    if(distance<=inner)return 255u;
    if(distance>=outer)return 0u;
    return 255u-(distance-inner)*255u/(outer-inner);
}

extern "C" __global__ void gui_raster(
    unsigned int *dst, const unsigned char *source,
    const kg2d_command_t *commands, unsigned long long source_bytes,
    unsigned int width, unsigned int height, unsigned int pitch_pixels,
    unsigned int command_count, unsigned int clip_x, unsigned int clip_y,
    unsigned int clip_width, unsigned int clip_height)
{
    unsigned long long tx = (unsigned long long)blockIdx.x * blockDim.x + threadIdx.x;
    unsigned long long ty = (unsigned long long)blockIdx.y * blockDim.y + threadIdx.y;
    unsigned long long x = clip_x + tx, y = clip_y + ty;
    if (tx >= clip_width || ty >= clip_height || x >= width || y >= height ||
        pitch_pixels < width || command_count > KG2D_MAX_COMMANDS) return;
    unsigned long long index = y * pitch_pixels + x;
    unsigned int pixel = dst[index];
    for (unsigned int i = 0; i < command_count; i++) {
        const kg2d_command_t c = commands[i];
        long long rx = (long long)x - c.x, ry = (long long)y - c.y;
        if (c.width <= 0 || c.height <= 0 || rx < 0 || ry < 0 ||
            rx >= c.width || ry >= c.height || c.opacity > 255u ||
            (c.op != KG2D_IMAGE_SCALED && c.op != KG2D_ARGB_BILINEAR &&
             (c.reserved[0] || c.reserved[1])) ||
            c.reserved[2] || c.reserved[3]) continue;
        unsigned int colour = c.colour0, opacity = c.opacity;
        if (c.op == KG2D_GRADIENT_H || c.op == KG2D_GRADIENT_V) {
            unsigned int extent = c.op == KG2D_GRADIENT_H ? c.width : c.height;
            unsigned long long at = c.op == KG2D_GRADIENT_H ? rx : ry;
            unsigned int t = extent > 1u ? (unsigned int)(at * 255u / (extent - 1u)) : 0;
            colour = widget_mix(c.colour0, c.colour1, t);
        } else if (c.op == KG2D_LINE) {
            if(c.colour1>3u || c.source_x || c.source_y || c.source_stride || c.source_offset)continue;
            unsigned dx=(unsigned)c.width-1u,dy=(unsigned)c.height-1u;
            unsigned u=c.colour1&1u?dx-(unsigned)rx:(unsigned)rx;
            unsigned v=c.colour1&2u?dy-(unsigned)ry:(unsigned)ry;
            /* Closed form of libgui's STRICT Bresenham tie comparisons.
             * Preserve the original endpoint direction, including half-step
             * ties. Wide products permit all positive signed-int extents. */
            if(dx>=dy) {
                if(v!=(dx?((unsigned long long)u*dy+(dx-1u)/2u)/dx:0u))continue;
            } else if(u!=((unsigned long long)v*dx+(dy-1u)/2u)/dy)continue;
        } else if (c.op == KG2D_LINE_AA) {
            if(c.colour1>3u || c.source_x || c.source_y || c.source_stride || c.source_offset)continue;
            bool steep=c.colour1&1u;
            int major=(steep?c.height:c.width)-1,minor=(steep?c.width:c.height)-2;
            if(minor<=0 || major<minor)continue;
            unsigned long long k=steep?ry:rx,v=steep?rx:ry;
            /* Normalize the major axis exactly as gui_line_aa. Quantize the
             * gradient BEFORE multiplication, retaining its endpoint fraction.
             * Coordinates relative to the minimum minor endpoint stay positive
             * even for negative slopes; no signed shifts or overflow needed. */
            unsigned long long step=((unsigned long long)minor*65536u)/(unsigned)major;
            unsigned long long fixed=c.colour1&2u?(unsigned long long)minor*65536u-k*step:k*step;
            unsigned long long whole=fixed>>16;
            unsigned lower=(unsigned)((fixed&65535u)*255u)>>16;
            unsigned coverage=v==whole?255u-lower:v==whole+1u?lower:0u;
            opacity=opacity*coverage/255u;
        } else if (c.op == KG2D_GLYPH) {
            if(!source || !c.source_x || !c.source_y || c.colour1>1u)continue;
            unsigned long long row_bytes=c.colour1?((unsigned long long)c.source_x+7u)/8u:c.source_x;
            if(row_bytes>c.source_stride)continue;
            unsigned long long gx=(unsigned long long)rx*c.source_x/(unsigned)c.width;
            unsigned long long gy=(unsigned long long)ry*c.source_y/(unsigned)c.height;
            unsigned long long off=(unsigned long long)c.source_offset+gy*c.source_stride+(c.colour1?gx/8u:gx);
            if(off>=source_bytes)continue;
            unsigned coverage=c.colour1?(source[off]&(0x80u>>(gx&7u))?255u:0u):source[off];
            if(coverage>=254u)coverage=255u;
            opacity=opacity*coverage/255u;
        } else if (c.op == KG2D_ROUNDED) {
            unsigned radius=c.source_x,style=c.source_y;
            bool top_only=style==2u || style==3u;
            if(radius>4096u || style>5u || radius>(unsigned)c.width/2u ||
               radius>(unsigned)c.height/(top_only?1u:2u) || c.source_stride || c.source_offset)continue;
            unsigned mx=(unsigned)rx,my=(unsigned)ry;
            if((unsigned)c.width-1u-mx<mx)mx=(unsigned)c.width-1u-mx;
            if(!top_only && (unsigned)c.height-1u-my<my)my=(unsigned)c.height-1u-my;
            unsigned coverage=255u;
            if(mx<radius && my<radius){
                if(style>=4u) {
                    /* Hard corners use the original pixel-centre convention,
                     * one pixel inside the AA circle. A frame keeps only the
                     * first covered pixel of each corner row, not a ring. */
                    unsigned dx=radius-mx-1u,dy=radius-my-1u;
                    bool inside=dx*dx+dy*dy<=radius*radius;
                    bool first=!mx || (dx+1u)*(dx+1u)+dy*dy>radius*radius;
                    coverage=inside && (style==4u || first)?255u:0u;
                } else {
                  coverage=rounded_coverage(radius-mx,radius-my,radius);
                  if(style==1u){
                    unsigned inner=rounded_coverage(radius-mx,radius-my,radius-1u);
                    coverage=coverage>inner+4u?coverage-inner:0u;
                  }else if(style==3u)coverage=coverage?255u:0u;
                }
            }else if(style==1u || style==5u)
                coverage=rx==0 || ry==0 || rx==c.width-1 || ry==c.height-1?255u:0u;
            if(style==3u){
                unsigned t=c.height>1?(unsigned)((unsigned long long)ry*255u/(c.height-1u)):0u;
                colour=widget_mix(c.colour0,c.colour1,t);
            }
            opacity=opacity*coverage/255u;
        } else if (c.op == KG2D_SHADOW) {
            unsigned spread=c.source_x;
            if (!spread || spread>4096u || c.source_y>4096u ||
                (unsigned)c.width<=2u*spread || (unsigned)c.height<=2u*spread+2u ||
                c.source_stride || c.source_offset) continue;
            long long bx=rx-spread, by=ry-spread-1;
            long long bw=c.width-2u*spread, bh=c.height-2u*spread-2u;
            unsigned dx=bx<0?-bx:bx>=bw?bx-bw+1:0;
            unsigned dy=by<0?-by:by>=bh?by-bh+1:0;
            if (!dx && !dy) continue;
            unsigned distance=dx>dy?dx+dy/2:dy+dx/2;
            if (dx && dy) distance+=c.source_y/3;
            if (distance>=spread) continue;
            unsigned remaining=spread-distance;
            opacity=(unsigned)((unsigned long long)opacity*remaining*remaining/(spread*spread));
        } else if (c.op == KG2D_ARGB_BILINEAR) {
            unsigned sw=c.reserved[0], sh=c.reserved[1];
            if (!source || !sw || !sh || c.colour1>1u) continue;
            unsigned long long ax=(unsigned long long)rx*(sw-1u), ay=(unsigned long long)ry*(sh-1u);
            unsigned dx=c.width>1?c.width-1:1, dy=c.height>1?c.height-1:1;
            unsigned long long qx=(ax/dx)*256u+(ax%dx)*256u/dx;
            unsigned long long qy=(ay/dy)*256u+(ay%dy)*256u/dy;
            unsigned fx=qx&255u, fy=qy&255u;
            unsigned long long sx=(unsigned long long)c.source_x+(qx>>8);
            unsigned long long sy=(unsigned long long)c.source_y+(qy>>8);
            if (sx+!!fx>0xffffffffull || sy+!!fy>0xffffffffull ||
                (sx+!!fx+1u)*4u>c.source_stride) continue;
            unsigned long long off=(unsigned long long)c.source_offset+sy*c.source_stride+sx*4u;
            unsigned long long last=off+(fy?c.source_stride:0u)+(fx?4u:0u);
            if(last>source_bytes || source_bytes-last<4u) continue;
            unsigned top=icon_lerp(icon_load(source+off),icon_load(source+off+(fx?4u:0u)),fx);
            off+=fy?c.source_stride:0u;
            unsigned bottom=icon_lerp(icon_load(source+off),icon_load(source+off+(fx?4u:0u)),fx);
            unsigned argb=icon_lerp(top,bottom,fy);
            opacity=(argb>>24)*opacity/255u;
            colour=(c.colour1?c.colour0:argb)&0xffffffu;
        } else if (c.op == KG2D_IMAGE || c.op == KG2D_IMAGE_SCALED || c.op == KG2D_MASK_A8) {
            unsigned int bpp = c.op == KG2D_MASK_A8 ? 1u : 4u;
            if (c.op == KG2D_IMAGE_SCALED) {
                if (!c.reserved[0] || !c.reserved[1]) continue;
                rx = (unsigned long long)rx * c.reserved[0] / (unsigned)c.width;
                ry = (unsigned long long)ry * c.reserved[1] / (unsigned)c.height;
            }
            unsigned long long sx = (unsigned long long)c.source_x + rx;
            unsigned long long sy = (unsigned long long)c.source_y + ry;
            /* Offset math is bounded before multiplying: all records use u32
             * offsets/strides and signed-positive extents. */
            if (!source || sx > 0xffffffffull || sy > 0xffffffffull ||
                sx * bpp + bpp > c.source_stride) continue;
            unsigned long long offset = (unsigned long long)c.source_offset +
                                         sy * c.source_stride + sx * bpp;
            if (offset > source_bytes || source_bytes - offset < bpp) continue;
            if (c.op != KG2D_MASK_A8) {
                /* Byte loads allow source subrectangles without alignment UB. */
                colour = (unsigned int)source[offset] |
                         ((unsigned int)source[offset + 1u] << 8) |
                         ((unsigned int)source[offset + 2u] << 16) |
                         ((unsigned int)source[offset + 3u] << 24);
            } else {
                opacity = (unsigned int)source[offset] * opacity / 255u;
            }
        } else if (c.op != KG2D_SOLID) {
            continue;
        }
        pixel = widget_mix(pixel, colour, opacity);
    }
    dst[index] = pixel;
}
