/* Real-GPU shader validation under the host NVIDIA driver. This does NOT test
 * Kestrel's RM/QMD submission, surface lifecycle, or desktop integration. */
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "gui_raster.cu"

#define CUDA_OK(call) do { cudaError_t e = (call); if (e != cudaSuccess) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(e)); \
    std::exit(1); } } while (0)

static uint32_t reference_mix(uint32_t a, uint32_t b, unsigned alpha) {
    if (!alpha) return a;
    if (alpha == 255) return b;
    unsigned red = (((a >> 16) & 255) * (255 - alpha) + ((b >> 16) & 255) * alpha) / 255;
    unsigned green = (((a >> 8) & 255) * (255 - alpha) + ((b >> 8) & 255) * alpha) / 255;
    unsigned blue = ((a & 255) * (255 - alpha) + (b & 255) * alpha) / 255;
    return (red << 16) | (green << 8) | blue;
}

static unsigned next_random(unsigned &rng) { return rng = rng * 1664525u + 1013904223u; }
static uint64_t exercised[9],rounded_styles[6];

static unsigned reference_corner(int64_t dx,int64_t dy,unsigned radius) {
    int64_t lo=(int64_t(radius)-1)*(int64_t(radius)-1);
    int64_t hi=(int64_t(radius)+1)*(int64_t(radius)+1),d=dx*dx+dy*dy;
    if(d<=lo)return 255;
    if(d>=hi)return 0;
    return unsigned(255-(d-lo)*255/(hi-lo));
}

static void run_case(unsigned w, unsigned h, unsigned pitch, unsigned count,
                     unsigned clip_x, unsigned clip_y, unsigned clip_w, unsigned clip_h,
                     unsigned seed) {
    const unsigned source_pitch = 131, source_rows = 67;
    std::vector<unsigned char> src(source_pitch * source_rows);
    for (auto &p : src) p = next_random(seed) >> 16;
    std::vector<kg2d_command_t> commands(count);
    for (unsigned i = 0; i < count; i++) {
        auto &c = commands[i];
        c.x = int(next_random(seed) % (w + 50)) - 25;
        c.y = int(next_random(seed) % (h + 50)) - 25;
        c.width = 1 + next_random(seed) % (w + 30);
        c.height = 1 + next_random(seed) % (h + 30);
        c.op = i % 9; c.colour0 = next_random(seed); c.colour1 = next_random(seed);
        c.opacity = (i/6) % 3 == 0 ? 255 : (i/6) % 3 == 1 ? 0 : next_random(seed) % 256;
        c.source_stride = source_pitch; c.source_offset = i % 3;
        c.source_x = next_random(seed) % 40; c.source_y = next_random(seed) % 15;
        if(c.op==KG2D_IMAGE_SCALED){c.reserved[0]=17;c.reserved[1]=29;c.opacity=173;}
        if(c.op==KG2D_SHADOW){c.source_x=1+next_random(seed)%42;c.source_y=next_random(seed)%24;
            c.source_stride=c.source_offset=0;c.opacity=90;}
        if(c.op==KG2D_ROUNDED){
            c.source_y=(i/9)%6;c.source_x=std::min<unsigned>(next_random(seed)%42,
                std::min<unsigned>(c.width/2,c.height/(c.source_y==2 || c.source_y==3?1:2)));
            c.source_stride=c.source_offset=0;c.opacity=173;
        }
        if(c.op==KG2D_ARGB_BILINEAR){
            c.reserved[0]=1+next_random(seed)%17;c.reserved[1]=1+next_random(seed)%29;
            c.source_x=next_random(seed)%15;c.colour1=i%2;
        }
    }
    if (count) {
        commands[0] = {}; commands[0].width = w; commands[0].height = h;
        commands[0].op = KG2D_GRADIENT_V; commands[0].colour0 = 0xff804020;
        commands[0].colour1 = 0x00facdb0; commands[0].opacity = 255;
    }
    if (count >= 9) {
        commands[count-1].x = INT32_MIN; commands[count-1].width = INT32_MAX;
        commands[count-2].source_x = UINT32_MAX; commands[count-2].op = KG2D_IMAGE;
        commands[count-3].source_y = UINT32_MAX; commands[count-3].op = KG2D_MASK_A8;
        commands[count-4].width = 0;
        commands[count-5].height = -1;
        commands[count-6].opacity = 256;
        commands[count-7].op = 0xffffffffu;
        commands[count-8].reserved[2] = 1;
    }
    std::vector<uint32_t> initial((size_t)pitch * (h + 2), 0xa55a1234), expected = initial;
    /* Reference paints command-by-command into a clipped CPU canvas. GPU uses
     * the independent pixel-by-pixel order. Guard rows and padding are included. */
    if (count <= KG2D_MAX_COMMANDS && pitch >= w) for (const auto &c : commands) {
        if (c.width <= 0 || c.height <= 0 || c.opacity > 255 || c.op > KG2D_ROUNDED ||
            (c.op!=KG2D_IMAGE_SCALED && c.op!=KG2D_ARGB_BILINEAR && (c.reserved[0] || c.reserved[1])) ||
            c.reserved[2] || c.reserved[3]) continue;
        int64_t x0 = std::max<int64_t>(clip_x, c.x), y0 = std::max<int64_t>(clip_y, c.y);
        int64_t x1 = std::min<int64_t>(std::min<uint64_t>(w, uint64_t(clip_x) + clip_w), int64_t(c.x) + c.width);
        int64_t y1 = std::min<int64_t>(std::min<uint64_t>(h, uint64_t(clip_y) + clip_h), int64_t(c.y) + c.height);
        for (int64_t y = y0; y < y1; y++) for (int64_t x = x0; x < x1; x++) {
            uint32_t colour = c.colour0; unsigned alpha = c.opacity;
            if (c.op == KG2D_GRADIENT_H || c.op == KG2D_GRADIENT_V) {
                int64_t p = c.op == KG2D_GRADIENT_H ? x-c.x : y-c.y;
                int n = c.op == KG2D_GRADIENT_H ? c.width : c.height;
                colour = reference_mix(c.colour0, c.colour1, n > 1 ? unsigned(p*255/(n-1)) : 0);
            } else if(c.op==KG2D_ROUNDED) {
                unsigned r=c.source_x,kind=c.source_y;
                bool top_only=kind==2 || kind==3;
                if(r>4096 || kind>5 || r>unsigned(c.width/2) ||
                   r>unsigned(c.height/(top_only?1:2)) || c.source_stride || c.source_offset)continue;
                int64_t u=x-c.x,v=y-c.y;
                int64_t corner_x=u<r?r-u:u>=c.width-r?u-(c.width-r)+1:0;
                int64_t corner_y=v<r?r-v:!top_only && v>=c.height-r?v-(c.height-r)+1:0;
                unsigned coverage=255;
                if(corner_x && corner_y){
                    if(kind>=4){
                        int64_t dx=corner_x-1,dy=corner_y-1;
                        coverage=dx*dx+dy*dy<=int64_t(r)*r?255:0;
                        if(kind==5){
                            // Independent original row search, not the shader's
                            // test of the adjacent point outside the arc.
                            int64_t first=-1;
                            for(unsigned j=0;j<r;j++){
                                int64_t d=int64_t(r)-j-1;
                                if(d*d+dy*dy<=int64_t(r)*r){first=j;break;}
                            }
                            coverage=int64_t(r)-corner_x==first?255:0;
                        }
                    }else{
                        coverage=reference_corner(corner_x,corner_y,r);
                        if(kind==1){unsigned inner=reference_corner(corner_x,corner_y,r-1);
                            coverage=coverage>inner+4?coverage-inner:0;}
                        if(kind==3)coverage=coverage?255:0;
                    }
                }else if(kind==1 || kind==5)coverage=u==0||v==0||u==c.width-1||v==c.height-1?255:0;
                if(kind==3)colour=reference_mix(c.colour0,c.colour1,c.height>1?unsigned(v*255/(c.height-1)):0);
                alpha=alpha*coverage/255;rounded_styles[kind]+=alpha!=0;
            } else if(c.op==KG2D_SHADOW) {
                int spread=c.source_x;
                if(spread<=0 || spread>4096 || c.source_y>4096 ||
                   c.width<=2*spread || c.height<=2*spread+2 || c.source_stride || c.source_offset)continue;
                int64_t left=int64_t(c.x)+spread,top=int64_t(c.y)+spread+1;
                int64_t right=int64_t(c.x)+c.width-spread,bottom=int64_t(c.y)+c.height-spread-1;
                int dx=x<left?left-x:x>=right?x-right+1:0;
                int dy=y<top?top-y:y>=bottom?y-bottom+1:0;
                if(!dx && !dy)continue;
                int distance=std::max(dx,dy)+std::min(dx,dy)/2;
                if(dx && dy)distance+=c.source_y/3;
                if(distance>=spread)continue;
                alpha=uint64_t(alpha)*(spread-distance)*(spread-distance)/(spread*spread);
            } else if(c.op==KG2D_ARGB_BILINEAR) {
                if(!c.reserved[0]||!c.reserved[1]||c.colour1>1)continue;
                uint64_t qx=uint64_t(x-c.x)*(c.reserved[0]-1)*256/(c.width>1?c.width-1:1);
                uint64_t qy=uint64_t(y-c.y)*(c.reserved[1]-1)*256/(c.height>1?c.height-1:1);
                unsigned fx=qx%256,fy=qy%256;
                uint64_t sx=c.source_x+qx/256,sy=c.source_y+qy/256;
                if(sx+!!fx>UINT32_MAX || sy+!!fy>UINT32_MAX || (sx+!!fx+1)*4>c.source_stride)continue;
                uint64_t off=c.source_offset+sy*c.source_stride+sx*4;
                uint64_t last=off+(fy?c.source_stride:0)+(fx?4:0);
                if(last>src.size() || src.size()-last<4)continue;
                unsigned argb=0;
                for(unsigned b=0;b<4;b++) {
                    unsigned top=(src[off+b]*(256-fx)+src[off+b+(fx?4:0)]*fx)/256;
                    unsigned bottom=(src[off+b+(fy?c.source_stride:0)]*(256-fx)+
                        src[off+b+(fy?c.source_stride:0)+(fx?4:0)]*fx)/256;
                    argb|=((top*(256-fy)+bottom*fy)/256)<<(b*8);
                }
                colour=(c.colour1?c.colour0:argb)&0xffffff;
                alpha=alpha*(argb>>24)/255;
            } else if (c.op == KG2D_IMAGE || c.op == KG2D_IMAGE_SCALED || c.op == KG2D_MASK_A8) {
                uint64_t sx = c.source_x + (x-c.x), sy = c.source_y + (y-c.y);
                if(c.op==KG2D_IMAGE_SCALED){
                    if(!c.reserved[0] || !c.reserved[1])continue;
                    sx=c.source_x+(x-c.x)*c.reserved[0]/c.width;
                    sy=c.source_y+(y-c.y)*c.reserved[1]/c.height;
                }
                unsigned bpp = c.op == KG2D_MASK_A8 ? 1 : 4;
                if (sx > UINT32_MAX || sy > UINT32_MAX || (sx+1)*bpp > c.source_stride) continue;
                uint64_t off = c.source_offset + sy*c.source_stride + sx*bpp;
                if (off > src.size() || src.size()-off < bpp) continue;
                if (bpp == 4) colour = uint32_t(src[off]) | (uint32_t(src[off+1]) << 8) |
                                      (uint32_t(src[off+2]) << 16) | (uint32_t(src[off+3]) << 24);
                else alpha = src[off] * alpha / 255;
            }
            auto &pixel = expected[(size_t)(y+1)*pitch+x];
            if (alpha) exercised[c.op]++;
            pixel = reference_mix(pixel, colour, alpha);
        }
    }
    uint32_t *d_dst; unsigned char *d_src; kg2d_command_t *d_commands;
    CUDA_OK(cudaMalloc(&d_dst, initial.size()*4));
    CUDA_OK(cudaMalloc(&d_src, src.size()));
    CUDA_OK(cudaMalloc(&d_commands, std::max<size_t>(1, commands.size())*sizeof(kg2d_command_t)));
    CUDA_OK(cudaMemcpy(d_dst, initial.data(), initial.size()*4, cudaMemcpyHostToDevice));
    CUDA_OK(cudaMemcpy(d_src, src.data(), src.size(), cudaMemcpyHostToDevice));
    if (count) CUDA_OK(cudaMemcpy(d_commands, commands.data(), commands.size()*sizeof(kg2d_command_t), cudaMemcpyHostToDevice));
    dim3 block(16,16), grid(std::max(1u,(clip_w+15)/16),std::max(1u,(clip_h+15)/16));
    gui_raster<<<grid,block>>>(d_dst+pitch,d_src,d_commands,src.size(),w,h,pitch,count,
                              clip_x,clip_y,clip_w,clip_h);
    CUDA_OK(cudaGetLastError()); CUDA_OK(cudaDeviceSynchronize());
    CUDA_OK(cudaMemcpy(initial.data(),d_dst,initial.size()*4,cudaMemcpyDeviceToHost));
    for (size_t i = 0; i < initial.size(); i++) if (initial[i] != expected[i]) {
        std::fprintf(stderr,"FAIL %ux%u/%u commands:%u offset:%zu got:%08x expected:%08x\n",
                     w,h,pitch,count,i,initial[i],expected[i]); std::exit(1);
    }
    CUDA_OK(cudaFree(d_commands)); CUDA_OK(cudaFree(d_src)); CUDA_OK(cudaFree(d_dst));
    std::printf("PASS %ux%u pitch:%u commands:%u clip:%u,%u+%ux%u (all pixels and guards)\n",
                w,h,pitch,count,clip_x,clip_y,clip_w,clip_h);
}

static void run_lines() {
    const unsigned w=128,h=96,pitch=144;
    const int extents[]={0,1,2,3,4,5,8,15,16,31,64,95,127};
    uint32_t *dst;kg2d_command_t *record;
    std::vector<uint32_t> initial(pitch*(h+2),0xa55a1234),expected,got(initial.size());
    CUDA_OK(cudaMalloc(&dst,initial.size()*4));CUDA_OK(cudaMalloc(&record,sizeof(kg2d_command_t)));
    unsigned cases=0;
    for(unsigned aa=0;aa<2;aa++)for(unsigned clipped=0;clipped<2;clipped++)for(unsigned direction=0;direction<4;direction++)
    for(int dx:extents)for(int dy:extents) {
        unsigned cx=clipped?3:0,cy=clipped?5:0,cw=clipped?100:w,ch=clipped?70:h;
        kg2d_command_t c={};c.x=clipped?-13:7;c.y=clipped?-9:11;
        c.width=dx+1;c.height=dy+1;c.op=KG2D_LINE;c.colour0=0xabcdef;
        c.colour1=direction;c.opacity=cases%2?133:255;
        if(aa&&dx&&dy){bool steep=dy>dx;c.op=KG2D_LINE_AA;
            c.width+=steep;c.height+=!steep;
            c.colour1=(steep?1u:0u)|(((direction&1)!=0)!=((direction&2)!=0)?2u:0u);}
        int x=c.x+(direction&1?dx:0),y=c.y+(direction&2?dy:0);
        int endx=c.x+(direction&1?0:dx),endy=c.y+(direction&2?0:dy);
        int sx=direction&1?-1:1,sy=direction&2?-1:1,err=dx-dy;
        expected=initial;
        // Independent iterative references, not the shader's per-pixel formula.
        if(c.op==KG2D_LINE_AA) {
            bool steep=dy>dx;
            if(steep){std::swap(x,y);std::swap(endx,endy);}
            if(x>endx){std::swap(x,endx);std::swap(y,endy);}
            int64_t step=int64_t(endy-y)*65536/(endx-x),fixed=int64_t(y)*65536;
            for(int u=x;u<=endx;u++,fixed+=step) {
                int64_t whole=fixed>=0?fixed/65536:-((-fixed+65535)/65536);
                unsigned lower=unsigned((fixed-whole*65536)*255/65536);
                for(unsigned offset=0;offset<2;offset++) {
                    int px=steep?int(whole)+offset:u,py=steep?u:int(whole)+offset;
                    if(px>=int(cx)&&py>=int(cy)&&px<int(cx+cw)&&py<int(cy+ch)&&px<int(w)&&py<int(h)) {
                        auto &pixel=expected[size_t(py+1)*pitch+px];
                        pixel=reference_mix(pixel,c.colour0,(offset?lower:255-lower)*c.opacity/255);
                    }
                }
            }
        } else for(;;) {
            if(x>=int(cx)&&y>=int(cy)&&x<int(cx+cw)&&y<int(cy+ch)&&x<int(w)&&y<int(h)) {
                auto &pixel=expected[size_t(y+1)*pitch+x];
                pixel=reference_mix(pixel,c.colour0,c.opacity);
            }
            if(x==endx&&y==endy)break;
            int twice=err*2;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;y+=sy;}
        }
        CUDA_OK(cudaMemcpy(dst,initial.data(),initial.size()*4,cudaMemcpyHostToDevice));
        CUDA_OK(cudaMemcpy(record,&c,sizeof c,cudaMemcpyHostToDevice));
        gui_raster<<<dim3((cw+15)/16,(ch+15)/16),dim3(16,16)>>>(
            dst+pitch,nullptr,record,0,w,h,pitch,1,cx,cy,cw,ch);
        CUDA_OK(cudaGetLastError());CUDA_OK(cudaDeviceSynchronize());
        CUDA_OK(cudaMemcpy(got.data(),dst,got.size()*4,cudaMemcpyDeviceToHost));
        if(got!=expected){std::fprintf(stderr,"FAIL line aa=%u dx=%d dy=%d direction=%u clipped=%u\n",aa,dx,dy,direction,clipped);std::exit(1);}
        cases++;
    }
    for(unsigned aa=0;aa<2;aa++)for(unsigned bad=0;bad<(aa?11u:8u);bad++) {
        kg2d_command_t c={};c.x=1;c.y=2;c.width=21;c.height=11;c.op=aa?KG2D_LINE_AA:KG2D_LINE;c.opacity=255;c.colour0=0xffffff;
        switch(bad){case 0:c.colour1=4;break;case 1:c.source_x=1;break;case 2:c.source_y=1;break;
        case 3:c.source_stride=4;break;case 4:c.source_offset=4;break;case 5:c.reserved[0]=1;break;
        case 6:c.reserved[3]=1;break;case 7:c.opacity=256;break;case 8:c.width=1;break;
        case 9:c.height=2;break;case 10:c.width=2;c.height=21;break;}
        CUDA_OK(cudaMemcpy(dst,initial.data(),initial.size()*4,cudaMemcpyHostToDevice));
        CUDA_OK(cudaMemcpy(record,&c,sizeof c,cudaMemcpyHostToDevice));
        gui_raster<<<dim3(8,6),dim3(16,16)>>>(dst+pitch,nullptr,record,0,w,h,pitch,1,0,0,w,h);
        CUDA_OK(cudaGetLastError());CUDA_OK(cudaDeviceSynchronize());
        CUDA_OK(cudaMemcpy(got.data(),dst,got.size()*4,cudaMemcpyDeviceToHost));
        if(got!=initial){std::fprintf(stderr,"FAIL malformed line %u\n",bad);std::exit(1);}
    }
    CUDA_OK(cudaFree(record));CUDA_OK(cudaFree(dst));
    std::printf("PASS GPU hard/AA lines: %u cases, every octant/reversed endpoints, points/axes/ties, negative slopes and fractional endpoints, clipped alpha, all pixels/padding/guards; 19 malformed commands rejected\n",cases);
}

static void run_glyphs() {
    const unsigned w=96,h=48,pitch=112;
    uint32_t *dst;unsigned char *source;kg2d_command_t *record;
    std::vector<uint32_t> initial(pitch*(h+2),0xa55a1234),expected,got(initial.size());
    std::vector<unsigned char> src(256);for(unsigned i=0;i<src.size();i++)src[i]=(i*73+19)&255;
    const unsigned widths[]={1,7,8,9,17};unsigned cases=0;
    CUDA_OK(cudaMalloc(&dst,initial.size()*4));CUDA_OK(cudaMalloc(&source,src.size()));
    CUDA_OK(cudaMalloc(&record,sizeof(kg2d_command_t)));
    for(unsigned bitmap=0;bitmap<2;bitmap++)for(unsigned gw:widths)
    for(unsigned scale=1;scale<=4;scale++)for(unsigned clip=0;clip<2;clip++) {
        unsigned gh=5,row=bitmap?(gw+7)/8:gw,stride=row+2,offset=3;
        src[offset]=254;src[offset+1]=255;src[offset+stride]=0;src[offset+stride+1]=127;
        kg2d_command_t c={};c.x=-2;c.y=1;c.width=gw*scale;c.height=gh*scale;
        c.op=KG2D_GLYPH;c.colour0=0xabcdef;c.colour1=bitmap;c.opacity=clip?173:255;
        c.source_x=gw;c.source_y=gh;c.source_stride=stride;c.source_offset=offset;
        unsigned cx=clip?3:0,cy=clip?4:0,cw=clip?37:w,ch=clip?15:h;
        expected=initial;
        // Independent source-first expansion, like the original font renderer.
        for(unsigned y=0;y<gh;y++)for(unsigned x=0;x<gw;x++) {
            unsigned a=bitmap?(src[offset+y*stride+x/8]&(0x80>>(x%8))?255:0):src[offset+y*stride+x];
            if(a>=254)a=255;
            for(unsigned yy=0;yy<scale;yy++)for(unsigned xx=0;xx<scale;xx++) {
                int px=c.x+x*scale+xx,py=c.y+y*scale+yy;
                if(px<int(cx)||py<int(cy)||px>=int(cx+cw)||py>=int(cy+ch)||px>=int(w)||py>=int(h))continue;
                auto &pixel=expected[size_t(py+1)*pitch+px];pixel=reference_mix(pixel,c.colour0,a*c.opacity/255);
            }
        }
        CUDA_OK(cudaMemcpy(dst,initial.data(),initial.size()*4,cudaMemcpyHostToDevice));
        CUDA_OK(cudaMemcpy(source,src.data(),src.size(),cudaMemcpyHostToDevice));
        CUDA_OK(cudaMemcpy(record,&c,sizeof c,cudaMemcpyHostToDevice));
        gui_raster<<<dim3((cw+15)/16,(ch+15)/16),dim3(16,16)>>>(
            dst+pitch,source,record,offset+(gh-1)*stride+row,w,h,pitch,1,cx,cy,cw,ch);
        CUDA_OK(cudaGetLastError());CUDA_OK(cudaDeviceSynchronize());
        CUDA_OK(cudaMemcpy(got.data(),dst,got.size()*4,cudaMemcpyDeviceToHost));
        if(got!=expected){std::fprintf(stderr,"FAIL glyph bitmap=%u width=%u scale=%u clip=%u\n",bitmap,gw,scale,clip);std::exit(1);}
        cases++;
    }
    for(unsigned bad=0;bad<7;bad++) {
        kg2d_command_t c={};c.x=1;c.y=2;c.width=21;c.height=11;c.op=KG2D_GLYPH;
        c.opacity=255;c.colour0=0xffffff;c.source_x=9;c.source_y=3;c.source_stride=11;
        switch(bad){case 0:c.colour1=2;break;case 1:c.source_x=0;break;case 2:c.source_y=0;break;
        case 3:c.source_stride=1;break;case 4:c.source_offset=UINT32_MAX;break;
        case 5:c.reserved[0]=1;break;case 6:c.opacity=256;break;}
        CUDA_OK(cudaMemcpy(dst,initial.data(),initial.size()*4,cudaMemcpyHostToDevice));
        CUDA_OK(cudaMemcpy(record,&c,sizeof c,cudaMemcpyHostToDevice));
        gui_raster<<<dim3(6,3),dim3(16,16)>>>(dst+pitch,source,record,src.size(),w,h,pitch,1,0,0,w,h);
        CUDA_OK(cudaGetLastError());CUDA_OK(cudaDeviceSynchronize());
        CUDA_OK(cudaMemcpy(got.data(),dst,got.size()*4,cudaMemcpyDeviceToHost));
        if(got!=initial){std::fprintf(stderr,"FAIL malformed glyph %u\n",bad);std::exit(1);}
    }
    CUDA_OK(cudaFree(record));CUDA_OK(cudaFree(source));CUDA_OK(cudaFree(dst));
    std::printf("PASS GPU glyphs: %u A8/bitmap cases, scales 1-4, bit boundaries/padded rows/exact source end/offset, near-opaque coverage, clipped alpha and full pixels/guards; 7 malformed commands rejected\n",cases);
}

static void run_surface_clear(size_t bytes) {
    /* Exact CREATE geometry from nv_surface_create, including the 4 MiB
     * scratch allocation that times out in Kestrel. Keep guard words outside
     * the allocation and compare every word, not just the completion fence. */
    const unsigned width=4096, height=unsigned(bytes/16384), guard=64;
    if (!bytes || bytes%65536) std::exit(1);
    uint32_t *dst; kg2d_command_t *commands;
    CUDA_OK(cudaMalloc(&dst,bytes+guard*8));
    CUDA_OK(cudaMalloc(&commands,sizeof(kg2d_command_t)));
    CUDA_OK(cudaMemset(dst,0xa5,bytes+guard*8));
    kg2d_command_t clear={};
    clear.width=width;clear.height=height;clear.op=KG2D_SOLID;clear.opacity=255;
    CUDA_OK(cudaMemcpy(commands,&clear,sizeof clear,cudaMemcpyHostToDevice));
    cudaEvent_t start,end;
    CUDA_OK(cudaEventCreate(&start));CUDA_OK(cudaEventCreate(&end));
    CUDA_OK(cudaEventRecord(start));
    gui_raster<<<dim3(256,(height+15)/16),dim3(16,16)>>>(
        dst+guard,nullptr,commands,0,width,height,width,1,0,0,width,height);
    CUDA_OK(cudaGetLastError());CUDA_OK(cudaEventRecord(end));
    CUDA_OK(cudaEventSynchronize(end));
    float milliseconds=0;CUDA_OK(cudaEventElapsedTime(&milliseconds,start,end));
    std::vector<uint32_t> got(bytes/4+guard*2);
    CUDA_OK(cudaMemcpy(got.data(),dst,bytes+guard*8,cudaMemcpyDeviceToHost));
    for(size_t i=0;i<got.size();i++) {
        uint32_t want=i<guard || i>=guard+bytes/4 ? 0xa5a5a5a5u : 0u;
        if(got[i]!=want){std::fprintf(stderr,"FAIL surface clear %zu bytes at word %zu\n",bytes,i);std::exit(1);}
    }
    std::printf("PASS exact surface clear %zu bytes, %ux%u, %.3f ms CUDA-event time (all words + guards)\n",
                bytes,width,height,milliseconds);
    CUDA_OK(cudaEventDestroy(end));CUDA_OK(cudaEventDestroy(start));
    CUDA_OK(cudaFree(commands));CUDA_OK(cudaFree(dst));
}

int main() {
    cudaDeviceProp prop; CUDA_OK(cudaGetDeviceProperties(&prop,0));
    std::printf("Host CUDA shader validation on %s (SM %d.%d), NOT Kestrel driver validation\n",
                prop.name,prop.major,prop.minor);
    if (prop.major != 12) { std::fprintf(stderr,"Expected Blackwell consumer GPU\n"); return 1; }
    run_lines();
    run_glyphs();
    run_surface_clear(65536);
    run_surface_clear(4u*1024*1024);
    run_surface_clear((size_t(7680)*1440*4+65535)&~size_t(65535));
    run_case(1,1,5,64,0,0,1,1,1);
    run_case(53,37,64,256,0,0,53,37,42);
    run_case(321,239,336,64,7,11,309,220,123);
    run_case(853,479,864,64,0,0,853,479,0xabcd);
    run_case(1920,1080,1936,24,0,0,1920,1080,0x1bad);
    run_case(3840,2160,3856,5,0,0,3840,2160,0xcafe);
    run_case(53,37,64,0,0,0,53,37,4);
    run_case(53,37,64,257,0,0,53,37,5);
    run_case(53,37,64,64,0,0,0,0,6);
    run_case(53,37,64,64,UINT32_MAX,UINT32_MAX,16,16,7);
    run_case(53,37,52,64,0,0,53,37,8);
    for (unsigned op = 0; op < 9; op++) {
        if (!exercised[op]) { std::fprintf(stderr,"Operation %u was not exercised\n",op); return 1; }
        std::printf("operation %u: %llu nonzero-opacity reference pixels compared\n",op,
                    static_cast<unsigned long long>(exercised[op]));
    }
    for(unsigned kind=0;kind<6;kind++){
        if(!rounded_styles[kind]){std::fprintf(stderr,"Rounded style %u was not exercised\n",kind);return 1;}
        std::printf("rounded style %u: %llu nonzero-opacity reference pixels compared\n",kind,
            static_cast<unsigned long long>(rounded_styles[kind]));
    }
    std::puts("GPU widget shader PASS; surface allocation, Kestrel launch and desktop wiring remain unverified");
    return 0;
}
