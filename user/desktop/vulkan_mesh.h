/* Bounded, cached triangle-list assets for the Vulkan raster bridge.
 * Generated only when shape/density changes, never in the animation loop. */
#ifndef KESTREL_VULKAN_MESH_H
#define KESTREL_VULKAN_MESH_H
#define VK_DEMO_SHAPES 6u
#define VK_DEMO_MAX_DETAIL 3u
static unsigned vk_mesh_count(unsigned shape,unsigned detail) {
    if(shape>=VK_DEMO_SHAPES || detail>VK_DEMO_MAX_DETAIL)return 0;
    unsigned n=1u<<detail, u=16*n,v=8*n;
    switch(shape){
    case 0:return 36*n*n;
    case 1:case 3:return u*v*6;
    case 2:return 6*(12*n)*(12*n);
    case 4:return u*6;
    default:return u*12;
    }
}
static api_vertex_t vk_mesh_vertex(unsigned shape,unsigned face,float u,float v) {
    api_vertex_t p={0,0,0,.4f+.5f*u,.4f+.5f*v,.9f,1};
    const float tau=6.2831853071795864769f;
    if(shape==0){
        /* All six parameterizations have outward counter-clockwise winding. */
        float a=2*u-1,b=2*v-1;
        switch(face){
        case 0:p.x=a;p.y=b;p.z=1;break;
        case 1:p.x=-a;p.y=b;p.z=-1;break;
        case 2:p.x=-1;p.y=b;p.z=a;break;
        case 3:p.x=1;p.y=b;p.z=-a;break;
        case 4:p.x=a;p.y=1;p.z=-b;break;
        default:p.x=a;p.y=-1;p.z=b;break;
        }
        static const float c[6][3]={{1,.35f,.35f},{.35f,1,.45f},{.4f,.55f,1},
            {1,.85f,.3f},{1,.45f,.95f},{.35f,.95f,1}};
        p.r=c[face][0];p.g=c[face][1];p.b=c[face][2];
    }else if(shape==1){
        float a=u*tau,b=v*tau,r=1.1f+.45f*cosf(b);
        p.x=r*cosf(a);p.y=r*sinf(a);p.z=.45f*sinf(b);
    }else if(shape==2){
        p.x=(u-.5f)*3.4f;p.y=(v-.5f)*3.4f;
        p.z=.35f*sinf(p.x*3)*cosf(p.y*3);
    }else if(shape==3){
        /* Latitude south -> north, longitude CCW; duplicated pole vertices
         * are harmless degenerate triangles and are counted as submitted. */
        float a=u*tau,b=(v-.5f)*tau*.5f;
        p.x=1.4f*cosf(b)*cosf(a);p.y=1.4f*cosf(b)*sinf(a);p.z=1.4f*sinf(b);
    }else{
        float a=u*tau,r=shape==4?1-v:1;
        p.x=r*cosf(a);p.y=r*sinf(a);p.z=2*v-1;
    }
    return p;
}
static void vk_mesh_quad(api_vertex_t *p,api_vertex_t a,api_vertex_t b,api_vertex_t c,api_vertex_t d){
    p[0]=a;p[1]=b;p[2]=c;p[3]=a;p[4]=c;p[5]=d;
}
static bool vk_mesh_build(api_vertex_t *out,unsigned capacity,unsigned shape,unsigned detail){
    unsigned count=vk_mesh_count(shape,detail);
    if(!out || !count || capacity<count)return false;
    unsigned n=1u<<detail,u=shape==0?n:shape==2?12*n:16*n;
    unsigned v=shape==0?n:shape==2?12*n:8*n,at=0;
    if(shape>=4){
        for(unsigned i=0;i<u;i++){
            api_vertex_t a=vk_mesh_vertex(shape,0,(float)i/u,0);
            api_vertex_t b=vk_mesh_vertex(shape,0,(float)(i+1)/u,0);
            api_vertex_t c=vk_mesh_vertex(shape,0,(float)(i+1)/u,1);
            api_vertex_t d=vk_mesh_vertex(shape,0,(float)i/u,1);
            api_vertex_t bottom={0,0,-1,.5f,.7f,.9f,1};
            out[at++]=bottom;out[at++]=b;out[at++]=a;
            if(shape==4){out[at++]=a;out[at++]=b;out[at++]=c;}
            else{
                vk_mesh_quad(out+at,a,b,c,d);at+=6;
                api_vertex_t top={0,0,1,.5f,.7f,.9f,1};
                out[at++]=top;out[at++]=d;out[at++]=c;
            }
        }
    }else for(unsigned face=0;face<(shape==0?6u:1u);face++)
        for(unsigned j=0;j<v;j++)for(unsigned i=0;i<u;i++){
            vk_mesh_quad(out+at,
                vk_mesh_vertex(shape,face,(float)i/u,(float)j/v),
                vk_mesh_vertex(shape,face,(float)(i+1)/u,(float)j/v),
                vk_mesh_vertex(shape,face,(float)(i+1)/u,(float)(j+1)/v),
                vk_mesh_vertex(shape,face,(float)i/u,(float)(j+1)/v));at+=6;
        }
    return at==count;
}
#endif
