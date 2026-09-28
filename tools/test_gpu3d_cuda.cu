/* Independent command-major double-precision CPU reference vs the actual
 * pixel-major GPU kernel. All coverage, pixels, depth and guards are checked.
 * Does not call any production shader helper in the reference.
 */
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <limits>
#include "gl_raster.cu"
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);std::exit(1); } } while(0)
#define CUDA_OK(x) CHECK((x)==cudaSuccess)
static const unsigned W=71,H=49,P=84,DP=80,TW=7,TH=5,TP=11;
static unsigned seed=0x10203040;
static unsigned cases=0;
static unsigned rng() { return seed=seed*1664525u+1013904223u; }
static double clamp(double x) {return std::max(0.0,std::min(1.0,x));}
static bool compare(unsigned op,double a,double b) {
    switch(op) {
    case 0:return false;case 1:return a<b;case 2:return a==b;case 3:return a<=b;
    case 4:return a>b;case 5:return a!=b;case 6:return a>=b;default:return true;
    }
}
static double factor(unsigned op,double alpha) {
    const double f[]={0,1,alpha,1-alpha,1,0};return f[op];
}
static uint32_t pack(const std::array<double,4> &c) {
    return uint32_t(clamp(c[0])*255+0.5)*65536+
           uint32_t(clamp(c[1])*255+0.5)*256+uint32_t(clamp(c[2])*255+0.5);
}
static std::array<double,4> fetch(const std::vector<unsigned> &t,int x,int y,unsigned ws,unsigned wt) {
    if(!ws)x=((x%int(TW))+TW)%TW;else x=std::max(0,std::min(int(TW)-1,x));
    if(!wt)y=((y%int(TH))+TH)%TH;else y=std::max(0,std::min(int(TH)-1,y));
    unsigned p=t[TP+y*TP+x];return {((p>>16)&255)/255.0,((p>>8)&255)/255.0,(p&255)/255.0,(p>>24)/255.0};
}
static std::array<double,4> sample(const std::vector<unsigned> &t,double u,double v,const kg3d_command_t &c) {
    if(!c.wrap_s)u-=std::floor(u);else u=clamp(u);
    if(!c.wrap_t)v-=std::floor(v);else v=clamp(v);
    u=u*TW-0.5;v=(1-v)*TH-0.5;
    if(!c.filter)return fetch(t,int(std::floor(u+0.5)),int(std::floor(v+0.5)),c.wrap_s,c.wrap_t);
    int x=int(std::floor(u)),y=int(std::floor(v));double fx=u-x,fy=v-y;
    auto a=fetch(t,x,y,c.wrap_s,c.wrap_t),b=fetch(t,x+1,y,c.wrap_s,c.wrap_t);
    auto d=fetch(t,x,y+1,c.wrap_s,c.wrap_t),e=fetch(t,x+1,y+1,c.wrap_s,c.wrap_t);
    std::array<double,4> out;
    for(unsigned k=0;k<4;k++)out[k]=a[k]*(1-fx)*(1-fy)+b[k]*fx*(1-fy)+d[k]*(1-fx)*fy+e[k]*fx*fy;
    return out;
}
struct point {int64_t x,y;};
static int64_t determinant(point a,point b,point c) {
    return a.x*(b.y-c.y)+b.x*(c.y-a.y)+c.x*(a.y-b.y);
}
static bool edge_in(int64_t d,point a,point b) {
    return d>0 || (d==0 && (b.y<a.y || (b.y==a.y && b.x<a.x)));
}
static uint64_t expected_fragments=0;
static void reference_fragment(const kg3d_command_t &c,std::array<double,4> rgba,
                               double z,unsigned &pixel,float &stored) {
    if(!std::isfinite(z) || z<0 || z>1 ||
       ((c.flags&KG3D_DEPTH_TEST) && !compare(c.depth_func,z,stored)))return;
    for(auto value:rgba)if(!std::isfinite(value))return;
    if((c.flags&KG3D_ALPHA_TEST) && !compare(c.alpha_func,rgba[3],c.alpha_ref))return;
    if(c.flags&KG3D_BLEND) {
        double sf=factor(c.blend_src,clamp(rgba[3])),df=factor(c.blend_dst,clamp(rgba[3]));
        for(int k=0;k<3;k++)rgba[k]=rgba[k]*sf+((pixel>>(16-k*8))&255)/255.0*df;
    }
    pixel=pack(rgba);if(c.flags&KG3D_DEPTH_WRITE)stored=float(z);expected_fragments++;
}
static void reference(std::vector<unsigned> &pixels,std::vector<float> &depth,
                      const std::vector<unsigned> &texture,const std::vector<kg3d_command_t> &cmd,
                      bool have_depth,bool have_texture,unsigned cx,unsigned cy,unsigned cw,unsigned ch) {
    for(const auto &c:cmd) {
        if(c.width<=0 || c.height<=0 || c.flags>KG3D_FLAGS || c.depth_func>7 || c.alpha_func>7 ||
           c.blend_src>5 || c.blend_dst>5 || c.tex_env>2 || c.wrap_s>1 || c.wrap_t>1 || c.filter>1 ||
           c.reserved[0] || c.reserved[1] || c.reserved[2] ||
           ((c.flags&67) && !have_depth))continue;
        unsigned primitive=c.flags&(KG3D_LINE|KG3D_POINT);
        if(primitive && (primitive==(KG3D_LINE|KG3D_POINT) || (c.flags&(16|32|64))))continue;
        bool clear=c.flags&96;
        point v[3];
        int order[3]={0,1,2};
        bool valid=std::isfinite(c.depth_bias) && std::isfinite(c.alpha_ref);
        for(int j=0;j<3;j++) {
            valid=valid && std::fabs(c.v[j].x)<=1048576 && std::fabs(c.v[j].y)<=1048576 &&
                std::isfinite(c.v[j].z) && c.v[j].inv_w>0 && std::isfinite(c.v[j].inv_w);
            if(std::isfinite(c.v[j].x) && std::isfinite(c.v[j].y)) {
                v[j]={int64_t(std::nearbyint(double(c.v[j].x)*16)),int64_t(std::nearbyint(double(c.v[j].y)*16))};
            } else v[j]={0,0};
        }
        if(!clear && (!valid || ((c.flags&16) && !have_texture)))continue;
        if(primitive) {
            /* Independent forward CPU DDA, not the GPU's inverse candidate
             * search. Preserve inclusive endpoints and duplicate-pixel blends. */
            const auto &a=c.v[0], &b=c.v[primitive==KG3D_POINT?0:1];
            float dx=b.x-a.x,dy=b.y-a.y;
            float extent=std::max(std::fabs(dx),std::fabs(dy));
            if(primitive==KG3D_LINE && extent>=8193)continue;
            int steps=std::max(1,int(extent));
            int last=primitive==KG3D_POINT?0:steps;
            for(int step=0;step<=last;step++) {
                float t=float(step)/steps;
                volatile float mx=dx*t,my=dy*t; /* no host FMA */
                int x=int(a.x+mx),y=int(a.y+my);
                if(x<0 || y<0 || x>=int(W) || y>=int(H) ||
                   x<int64_t(cx) || y<int64_t(cy) || uint64_t(x)-cx>=cw || uint64_t(y)-cy>=ch ||
                   int64_t(x)-c.x<0 || int64_t(y)-c.y<0 ||
                   int64_t(x)-c.x>=c.width || int64_t(y)-c.y>=c.height)continue;
                volatile float wi=(b.inv_w-a.inv_w)*t;
                float iw=a.inv_w+wi;if(!(iw>0) || !std::isfinite(iw))continue;
                float w=1.0f/iw;
                std::array<double,4> rgba;
                for(int k=0;k<4;k++){volatile float v=(b.rgba[k]-a.rgba[k])*t;rgba[k]=(a.rgba[k]+v)*w;}
                volatile float dz=(b.z-a.z)*t;float z=a.z+dz;
                reference_fragment(c,rgba,z,pixels[P+y*P+x],depth[DP+y*DP+x]);
            }
            continue;
        }
        int64_t area=determinant(v[0],v[1],v[2]);
        if(!clear && area==0)continue;
        if(area<0){std::swap(v[1],v[2]);std::swap(order[1],order[2]);area=-area;}
        for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++) {
            int64_t rx=int64_t(x)-c.x,ry=int64_t(y)-c.y;
            if(x<cx || y<cy || uint64_t(x)-cx>=cw || uint64_t(y)-cy>=ch ||
               rx<0 || ry<0 || rx>=c.width || ry>=c.height)continue;
            auto &pixel=pixels[P+y*P+x];auto &stored=depth[DP+y*DP+x];
            if(clear) {
                if(c.flags&32)pixel=pack({c.v[0].rgba[0],c.v[0].rgba[1],c.v[0].rgba[2],c.v[0].rgba[3]});
                if((c.flags&64) && std::isfinite(c.v[0].z))stored=float(clamp(c.v[0].z));
                continue;
            }
            point p={int64_t(x)*16+8,int64_t(y)*16+8};
            int64_t e[]={determinant(v[1],v[2],p),determinant(v[2],v[0],p),determinant(v[0],v[1],p)};
            if(!edge_in(e[0],v[1],v[2]) || !edge_in(e[1],v[2],v[0]) || !edge_in(e[2],v[0],v[1]))continue;
            double l[]={double(e[0])/area,double(e[1])/area,double(e[2])/area};
            const auto &a=c.v[order[0]], &b=c.v[order[1]], &d=c.v[order[2]];
            /* The attachment and incoming depth are float32, not double.
             * Comparing unrounded double against stored float invents a
             * LESS result for e.g. 0.4999999917, which is float32 0.5. */
            double z=float(a.z+(double(b.z)-a.z)*l[1]+(double(d.z)-a.z)*l[2]+c.depth_bias);
            double iw=a.inv_w+(double(b.inv_w)-a.inv_w)*l[1]+(double(d.inv_w)-a.inv_w)*l[2];
            if(!std::isfinite(z) || z<0 || z>1 || ((c.flags&1) && !compare(c.depth_func,z,stored)) ||
               !(iw>0) || !std::isfinite(iw))continue;
            std::array<double,4> rgba={0,0,0,0};
            for(int k=0;k<4;k++)
                rgba[k]=(a.rgba[k]+(double(b.rgba[k])-a.rgba[k])*l[1]+(double(d.rgba[k])-a.rgba[k])*l[2])/iw;
            if(c.flags&16) {
                double s=(a.uv[0]+(double(b.uv[0])-a.uv[0])*l[1]+(double(d.uv[0])-a.uv[0])*l[2])/iw;
                double t=(a.uv[1]+(double(b.uv[1])-a.uv[1])*l[1]+(double(d.uv[1])-a.uv[1])*l[2])/iw;
                if(!std::isfinite(s) || !std::isfinite(t))continue;
                auto texel=sample(texture,s,t,c);
                for(int k=0;k<4;k++) {
                    if(c.tex_env==1)rgba[k]=texel[k];
                    else if(c.tex_env==2){if(k<3)rgba[k]=rgba[k]*(1-texel[3])+texel[k]*texel[3];}
                    else rgba[k]*=texel[k];
                }
            }
            reference_fragment(c,rgba,z,pixel,stored);
        }
    }
}
static kg3d_command_t triangle() {
    kg3d_command_t c={};c.width=W;c.height=H;c.depth_func=1;c.alpha_func=7;
    c.blend_src=2;c.blend_dst=3;c.alpha_ref=0.43f;
    const float xy[3][2]={{3,2},{66,7},{14,46}};
    for(unsigned j=0;j<3;j++) {
        auto &v=c.v[j];v.x=xy[j][0];v.y=xy[j][1];v.z=0.25f;v.inv_w=1;
        v.rgba[0]=j==0?0.75f:0.125f;v.rgba[1]=j==1?0.75f:0.125f;v.rgba[2]=j==2?0.75f:0.125f;
        v.rgba[3]=0.625f;v.uv[0]=j==1?1.75f:-0.25f;v.uv[1]=j==2?1.25f:0.125f;
    }
    return c;
}
static void run(std::vector<kg3d_command_t> cmd,int mode=0,
                unsigned cx=0,unsigned cy=0,unsigned cw=W,unsigned ch=H) {
    std::vector<unsigned> initial(P*(H+2),0xc0abcdef),tex(TP*(TH+2),0xc0deadbe);
    std::vector<float> di(DP*(H+2),-1234);
    for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++){initial[P+y*P+x]=0x102030;di[DP+y*DP+x]=0.5f;}
    for(unsigned y=0;y<TH;y++)for(unsigned x=0;x<TW;x++)tex[TP+y*TP+x]=rng();
    auto expected=initial;auto de=di;
    if(mode!=3 && cmd.size()<=KG3D_MAX_COMMANDS)reference(expected,de,tex,cmd,mode!=1,mode!=2 && mode!=4,cx,cy,cw,ch);
    unsigned *dp,*tp;float *dd;kg3d_command_t *dc;
    CUDA_OK(cudaMalloc(&dp,initial.size()*4));CUDA_OK(cudaMalloc(&dd,di.size()*4));
    CUDA_OK(cudaMalloc(&tp,tex.size()*4));CUDA_OK(cudaMalloc(&dc,std::max(size_t(1),cmd.size())*sizeof(cmd[0])));
    CUDA_OK(cudaMemcpy(dp,initial.data(),initial.size()*4,cudaMemcpyHostToDevice));
    CUDA_OK(cudaMemcpy(dd,di.data(),di.size()*4,cudaMemcpyHostToDevice));
    CUDA_OK(cudaMemcpy(tp,tex.data(),tex.size()*4,cudaMemcpyHostToDevice));
    if(!cmd.empty())CUDA_OK(cudaMemcpy(dc,cmd.data(),cmd.size()*sizeof(cmd[0]),cudaMemcpyHostToDevice));
    gl_raster<<<dim3((W+15)/16,(H+15)/16),dim3(16,16)>>>(
        mode==3?nullptr:dp+P,mode==1?nullptr:dd+DP,mode==2?nullptr:tp+TP,dc,
        mode==4?((TH-1)*TP+TW)*4-1:TH*TP*4,W,H,P,DP,TW,TH,TP,unsigned(cmd.size()),cx,cy,cw,ch);
    CUDA_OK(cudaGetLastError());CUDA_OK(cudaDeviceSynchronize());
    auto actual=initial;auto da=di;auto ta=tex;
    CUDA_OK(cudaMemcpy(actual.data(),dp,actual.size()*4,cudaMemcpyDeviceToHost));
    CUDA_OK(cudaMemcpy(da.data(),dd,da.size()*4,cudaMemcpyDeviceToHost));
    CUDA_OK(cudaMemcpy(ta.data(),tp,ta.size()*4,cudaMemcpyDeviceToHost));
    for(unsigned i=0;i<actual.size();i++) {
        if(expected[i]==initial[i])CHECK(actual[i]==expected[i]);
        else {
            for(unsigned shift=0;shift<24;shift+=8) {
                if(std::abs(int((actual[i]>>shift)&255)-int((expected[i]>>shift)&255))>1) {
                    std::fprintf(stderr,"case %u pixel %u (%u,%u) expected %08x actual %08x\n",cases,i,i%P,i/P,expected[i],actual[i]);std::exit(1);
                }
            }
            CHECK((actual[i]>>24)==0);
        }
    }
    for(unsigned i=0;i<da.size();i++)CHECK(std::fabs(da[i]-de[i])<0.00001f);
    CHECK(ta==tex);
    CUDA_OK(cudaFree(dp));CUDA_OK(cudaFree(dd));CUDA_OK(cudaFree(tp));CUDA_OK(cudaFree(dc));cases++;
}
int main() {
    cudaDeviceProp props={};CUDA_OK(cudaGetDeviceProperties(&props,0));
    CHECK(props.major==12);std::printf("GPU: %s\n",props.name);
    auto base=triangle();
    run({base});std::swap(base.v[1],base.v[2]);run({base});base=triangle();
    for(unsigned df=0;df<8;df++)for(float z:{0.25f,0.5f,0.75f}) {
        auto c=base;c.flags=3;c.depth_func=df;for(auto &v:c.v)v.z=z;run({c});
    }
    for(unsigned af=0;af<8;af++) {
        auto c=base;c.flags=4;c.alpha_func=af;c.alpha_ref=0.5f;
        for(auto &v:c.v)v.rgba[3]=0.25f;run({c});
        for(auto &v:c.v)v.rgba[3]=0.5f;run({c});
        for(auto &v:c.v)v.rgba[3]=0.75f;run({c});
    }
    for(unsigned sf=0;sf<6;sf++)for(unsigned df=0;df<6;df++) {
        auto c=base;c.flags=8;c.blend_src=sf;c.blend_dst=df;run({c,c});
    }
    for(unsigned env=0;env<3;env++)for(unsigned ws=0;ws<2;ws++)
    for(unsigned wt=0;wt<2;wt++)for(unsigned filter=0;filter<2;filter++) {
        auto c=base;c.flags=16;c.tex_env=env;c.wrap_s=ws;c.wrap_t=wt;c.filter=filter;
        for(unsigned j=0;j<3;j++) {
            float iw=j==1?0.25f:j==2?2.0f:1.0f;c.v[j].inv_w=iw;
            for(auto &v:c.v[j].rgba)v*=iw;for(auto &v:c.v[j].uv)v*=iw;
        }
        run({c});c.flags|=8|4;run({c});
    }
    /* Exact, representable repeat/clamp boundaries with constant attributes.
     * Constant interpolation must preserve these even at shared edges. */
    for(unsigned ws=0;ws<2;ws++)for(float s:{-1.0f,0.0f,0.25f,0.5f,1.0f,2.0f}) {
        auto c=base;c.flags=16;c.tex_env=1;c.wrap_s=c.wrap_t=ws;
        for(auto &v:c.v){v.uv[0]=s;v.uv[1]=s;}
        run({c});
    }
    /* Adjacent triangles exactly cover the rectangle once: double blending or
     * a crack on the diagonal is an exact-value failure, not a visual check. */
    auto a=base,b=base;a.flags=b.flags=8;
    const float xy[4][2]={{8,8},{40,8},{40,40},{8,40}};
    for(int j=0;j<3;j++) {
        a.v[j].x=xy[j][0];a.v[j].y=xy[j][1];
        int k=j==0?0:j+1;b.v[j].x=xy[k][0];b.v[j].y=xy[k][1];
        for(int c=0;c<3;c++)a.v[j].rgba[c]=b.v[j].rgba[c]=0.75f;
    }
    run({a,b});run({b,a});
    /* Clear, draw, clear a subregion, then draw an overlapping near triangle. */
    auto clear=base;clear.flags=96;clear.v[0].z=1;
    auto near=base;near.flags=3;for(auto &v:near.v)v.z=0.125f;
    auto partial=clear;partial.x=17;partial.y=19;partial.width=11;partial.height=13;
    run({clear,base,partial,near});run({clear,near},0,11,7,43,31);
    run({clear,near},0,UINT32_MAX,UINT32_MAX,W,H);
    for(int mode=1;mode<=4;mode++) {auto c=base;c.flags=19;run({c},mode);}
    run({});run(std::vector<kg3d_command_t>(65,base));
    for(unsigned i=0;i<18;i++) {
        auto c=base;
        switch(i) {
        case 0:c.flags=512;break;case 1:c.width=0;break;case 2:c.height=-1;break;
        case 3:c.reserved[2]=1;break;case 4:c.depth_func=8;break;case 5:c.alpha_func=8;break;
        case 6:c.blend_src=6;break;case 7:c.blend_dst=6;break;case 8:c.filter=2;break;
        case 9:c.wrap_s=2;break;case 10:c.tex_env=3;break;case 11:c.v[0].x=INFINITY;break;
        case 12:c.v[0].inv_w=0;break;case 13:c.v[0].z=NAN;break;case 14:c.v[0].rgba[0]=NAN;break;
        case 15:c.v[1]=c.v[0];break;case 16:c.v[0].x=1048577.0f;break;
        case 17:c.flags=16;c.v[0].uv[0]=INFINITY;break;
        }
        run({c});
    }
    /* Maximum batch, many overlapping clipped triangles, non-power-of-two
     * surfaces and unequal colour/depth pitches. */
    std::vector<kg3d_command_t> random;
    for(unsigned i=0;i<64;i++) {
        /* Random nearest samples can land exactly on a texel boundary
         * (observed s=-2/7 for width 7). Adjacent choices from float32 vs
         * float64 rounding are legitimate, but obscure ordered-batch defects.
         * Use continuous linear sampling here; nearest is covered separately
         * above, including constant wrap boundaries and perspective inputs. */
        auto c=base;c.flags=(i%3==0?19:8);c.filter=KG3D_LINEAR;c.wrap_s=i%2;c.wrap_t=(i/2)%2;
        c.tex_env=i%3;
        for(auto &v:c.v) {
            v.x=float(int(rng()%120)-25);v.y=float(int(rng()%90)-20);
            /* Distinct shuffled depth intervals avoid an ill-conditioned
             * double-vs-FMA comparison within an ULP of another triangle or
             * the 0.5 clear plane. All equality operators have exact directed
             * cases above; each randomized triangle still has varying depth. */
            v.z=(float((i*37)%64)+0.2f+float(rng()%100)*0.002f)/64;
            v.inv_w=float(rng()%4+1)/4;
            for(auto &p:v.rgba)p=float(rng()%200+20)/255*v.inv_w;
            for(auto &p:v.uv)p=float(int(rng()%32)-16)/8*v.inv_w;
        }
        random.push_back(c);
    }
    for(unsigned n=1;n<=random.size();n++) {
        run(std::vector<kg3d_command_t>(random.begin(),random.begin()+n));
    }
    run(random,0,9,13,37,23);
    /* Points and all line octants, integer/fractional coordinates, clipping,
     * reversed endpoints, perspective colors, degenerate lines and long lines. */
    for(float ox:{-0.5f,0.0f,0.25f,0.5f})for(int dx:{-63,-17,0,17,63})
    for(int dy:{-45,-9,0,9,45}) {
        auto c=base;c.flags=KG3D_LINE|KG3D_BLEND;
        c.v[0].x=35+ox;c.v[0].y=24+ox;c.v[1].x=c.v[0].x+dx;c.v[1].y=c.v[0].y+dy;
        c.v[1].inv_w=0.25f;for(auto &value:c.v[1].rgba)value*=0.25f;
        run({c});std::swap(c.v[0],c.v[1]);run({c});
        c.flags|=KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE;run({c});
    }
    for(float x:{-1.0f,-0.5f,0.0f,0.5f,35.25f,70.5f,71.0f})
    for(float y:{-1.0f,-0.5f,0.0f,0.5f,24.25f,48.5f,49.0f}) {
        auto c=base;c.flags=KG3D_POINT|KG3D_BLEND;c.v[0].x=x;c.v[0].y=y;
        run({c,c});c.flags|=KG3D_DEPTH_TEST|KG3D_DEPTH_WRITE;run({c,c});
    }
    for(float len:{8192.0f,8192.5f,8193.0f}) {
        auto c=base;c.flags=KG3D_LINE|KG3D_BLEND;
        c.v[0].x=-4000;c.v[0].y=20.25f;c.v[1].x=-4000+len;c.v[1].y=27.75f;
        run({c});std::swap(c.v[0],c.v[1]);run({c});
    }
    for(unsigned flags:{KG3D_LINE|KG3D_POINT,KG3D_LINE|KG3D_TEXTURE,KG3D_POINT|KG3D_CLEAR_DEPTH}) {
        auto c=base;c.flags=flags;run({c});
    }
    CHECK(expected_fragments>100000);
    std::printf("PASS GPU application primitive shader: %u cases, %llu reference fragments; triangle/line/point coverage, perspective texture, depth/alpha/blend, ordering, clips and guards (Windows CUDA, not native Kestrel)\n",
                cases,(unsigned long long)expected_fragments);
}
