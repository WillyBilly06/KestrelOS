/* Desktop scene assets are generated once, then transformed/lit/textured by
 * GPU shader programs. CPU work per frame is camera uniforms and draw assembly,
 * not per-vertex lighting/transforms or pixel rasterisation. */
#include "app_gl_scene.h"
#include "GL.h"
#include "math.h"

typedef struct {float position[3],normal[3],colour[4],uv[2];} scene_vertex_t;
struct gl_scene {
    GLuint program,texture;
    GLuint ground_program;
    GLint ground_position,ground_colour,ground_projection,ground_modelview;
    GLint position,normal,colour,uv,projection,modelview,light,lighting,image,textured;
    scene_vertex_t *mesh;
    unsigned scene,count;
    scene_vertex_t ground[64*6];
};

static const char *scene_vertex_source =
    "attribute vec3 position; attribute vec3 normal; attribute vec4 colour; attribute vec2 uv;\n"
    "uniform mat4 projection; uniform mat4 modelview; uniform vec3 light; uniform float lighting;\n"
    "varying vec4 tint; varying vec2 texcoord;\n"
    "void main() {\n"
    "  vec4 eye = modelview * vec4(position, 1.0);\n"
    "  gl_Position = projection * eye; texcoord = uv; tint = colour;\n"
    "  if (lighting > 0.5) {\n"
    "    vec3 n = normalize((modelview * vec4(normal, 0.0)).xyz);\n"
    "    vec3 l = normalize(light - eye.xyz);\n"
    "    float diffuse = max(dot(n, l), 0.0);\n"
    "    vec3 halfway = normalize(l + normalize(-eye.xyz));\n"
    "    float specular = pow(max(dot(n, halfway), 0.0), 24.0);\n"
    "    if (diffuse <= 0.0) specular = 0.0;\n"
    "    tint.rgb = colour.rgb * (vec3(0.44, 0.48, 0.60) + vec3(diffuse)) + vec3(0.6 * specular);\n"
    "    tint.rgb = clamp(tint.rgb, 0.0, 1.0);\n"
    "  }\n"
    "}\n";
static const char *scene_fragment_source =
    "varying vec4 tint; varying vec2 texcoord; uniform sampler2D image; uniform float textured;\n"
    "void main() {\n"
    "  vec4 c = tint;\n"
    "  if (textured > 0.5) c = c * texture2D(image, texcoord);\n"
    "  gl_FragColor = c;\n"
    "}\n";

static bool scene_status(const char *phase) {
    GLenum error=glGetError();
    if(!error)return true;
    char note[128];snprintf(note,sizeof note,"%s failed: GL error 0x%x; no CPU fallback",phase,error);
    log_write(3,"3d-gpu",note);return false;
}

static GLuint scene_shader(GLenum stage,const char *source) {
    GLuint s=glCreateShader(stage);
    if(!s)return 0;
    glShaderSource(s,1,&source,NULL);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(ok)return s;
    char error[256];glGetShaderInfoLog(s,sizeof error,NULL,error);
    log_write(3,"3d-gpu",error);glDeleteShader(s);return 0;
}

static scene_vertex_t vertex(float x,float y,float z,float nx,float ny,float nz,
                              float r,float g,float b,float a,float u,float v) {
    scene_vertex_t out={{x,y,z},{nx,ny,nz},{r,g,b,a},{u,v}};return out;
}
static void quad(scene_vertex_t *out,scene_vertex_t a,scene_vertex_t b,
                  scene_vertex_t c,scene_vertex_t d) {
    out[0]=a;out[1]=b;out[2]=c;out[3]=a;out[4]=c;out[5]=d;
}
static scene_vertex_t torus_vertex(unsigned i,unsigned j) {
    float u=(float)i/32*(float)M_TAU,v=(float)j/16*(float)M_TAU;
    float cu=cosf(u),su=sinf(u),cv=cosf(v),sv=sinf(v);
    return vertex((1.1f+.45f*cv)*cu,(1.1f+.45f*cv)*su,.45f*sv,
                  cv*cu,cv*su,sv,.75f,.85f,1,1,u*4/(float)M_TAU,v*2/(float)M_TAU);
}
static scene_vertex_t terrain_vertex(unsigned x,unsigned y) {
    float fx=((float)x/24-.5f)*4.2f,fy=((float)y/24-.5f)*4.2f;
    float h=sinf(fx*3)*cosf(fy*3)*.35f;
    float dx=3*cosf(fx*3)*cosf(fy*3)*.35f,dy=-3*sinf(fx*3)*sinf(fy*3)*.35f;
    float n=sqrtf(dx*dx+dy*dy+1);
    return vertex(fx,fy,h,-dx/n,-dy/n,1/n,.35f+h,.65f+h*.5f,.9f-h,1,
                  (float)x/24*3,(float)y/24*3);
}

static bool scene_mesh(gl_scene_t *s,unsigned which) {
    if(which>=3)return false;
    if(s->mesh&&s->scene==which)return true;
    // Geometry is immutable between scene switches; release the unused mesh
    // before allocating its replacement to avoid a double-sized transient peak.
    free(s->mesh);s->mesh=NULL;s->count=0;
    unsigned count=which==0?36:which==1?32*16*6:24*24*6;
    s->mesh=malloc(count*sizeof(*s->mesh));if(!s->mesh)return false;
    s->scene=which;s->count=count;
    unsigned at=0;
    if(which==0){
        static const float p[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
            {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
        static const unsigned face[6][4]={{4,5,6,7},{1,0,3,2},{0,4,7,3},{5,1,2,6},{7,6,2,3},{0,1,5,4}};
        static const float normal[6][3]={{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
        static const float colour[6][3]={{1,.35f,.35f},{.35f,1,.45f},{.4f,.55f,1},
            {1,.85f,.3f},{1,.45f,.95f},{.35f,.95f,1}};
        for(unsigned f=0;f<6;f++){
            scene_vertex_t v[4];
            for(unsigned k=0;k<4;k++){
                const float *a=p[face[f][k]],*n=normal[f],*c=colour[f];
                v[k]=vertex(a[0],a[1],a[2],n[0],n[1],n[2],c[0],c[1],c[2],1,
                            k==1||k==2,k>=2);
            }
            quad(s->mesh+at,v[0],v[1],v[2],v[3]);at+=6;
        }
    }else if(which==1){
        for(unsigned i=0;i<32;i++)for(unsigned j=0;j<16;j++){
            quad(s->mesh+at,torus_vertex(i,j),torus_vertex(i+1,j),
                 torus_vertex(i+1,j+1),torus_vertex(i,j+1));at+=6;
        }
    }else{
        for(unsigned y=0;y<24;y++)for(unsigned x=0;x<24;x++){
            scene_vertex_t a=terrain_vertex(x,y+1),b=terrain_vertex(x,y),
                c=terrain_vertex(x+1,y+1),d=terrain_vertex(x+1,y);
            s->mesh[at++]=a;s->mesh[at++]=b;s->mesh[at++]=c;
            s->mesh[at++]=c;s->mesh[at++]=b;s->mesh[at++]=d;
        }
    }
    return at==count;
}

static bool scene_init(gl_scene_t *s) {
    GLuint v=scene_shader(GL_VERTEX_SHADER,scene_vertex_source);
    GLuint f=scene_shader(GL_FRAGMENT_SHADER,scene_fragment_source);
    if(!v||!f){glDeleteShader(v);glDeleteShader(f);return false;}
    s->program=glCreateProgram();
    if(s->program){glAttachShader(s->program,v);glAttachShader(s->program,f);glLinkProgram(s->program);}
    GLint ok=0;if(s->program)glGetProgramiv(s->program,GL_LINK_STATUS,&ok);
    glDeleteShader(v);glDeleteShader(f); // attachments survive until program destruction
    if(!ok){
        char note[256]="no program object";
        if(s->program)glGetProgramInfoLog(s->program,sizeof note,NULL,note);
        log_write(3,"3d-gpu",note);scene_status("link");return false;
    }
    s->position=glGetAttribLocation(s->program,"position");
    s->normal=glGetAttribLocation(s->program,"normal");
    s->colour=glGetAttribLocation(s->program,"colour");s->uv=glGetAttribLocation(s->program,"uv");
    s->projection=glGetUniformLocation(s->program,"projection");s->modelview=glGetUniformLocation(s->program,"modelview");
    s->light=glGetUniformLocation(s->program,"light");s->lighting=glGetUniformLocation(s->program,"lighting");
    s->image=glGetUniformLocation(s->program,"image");s->textured=glGetUniformLocation(s->program,"textured");
    if(s->position<0||s->normal<0||s->colour<0||s->uv<0||s->projection<0||s->modelview<0||
       s->light<0||s->lighting<0||s->image<0||s->textured<0)return false;
    /* The ground has neither lighting nor texture. Specialize that immutable
     * material instead of executing the lighting shader's untaken branches,
     * uploading normal/UV attributes and exporting unused varyings for it.
     * Positions, colours and rasterisation still execute on the GPU. */
    v=scene_shader(GL_VERTEX_SHADER,
        "attribute vec3 position; attribute vec4 colour; uniform mat4 projection; uniform mat4 modelview;"
        "varying vec4 tint; void main(){gl_Position=projection*(modelview*vec4(position,1.0));tint=colour;}");
    f=scene_shader(GL_FRAGMENT_SHADER,
        "varying vec4 tint; void main(){gl_FragColor=tint;}");
    if(!v||!f){glDeleteShader(v);glDeleteShader(f);return false;}
    s->ground_program=glCreateProgram();
    if(s->ground_program){glAttachShader(s->ground_program,v);glAttachShader(s->ground_program,f);glLinkProgram(s->ground_program);}
    ok=0;if(s->ground_program)glGetProgramiv(s->ground_program,GL_LINK_STATUS,&ok);
    glDeleteShader(v);glDeleteShader(f);
    if(!ok)return false;
    s->ground_position=glGetAttribLocation(s->ground_program,"position");
    s->ground_colour=glGetAttribLocation(s->ground_program,"colour");
    s->ground_projection=glGetUniformLocation(s->ground_program,"projection");
    s->ground_modelview=glGetUniformLocation(s->ground_program,"modelview");
    if(s->ground_position<0||s->ground_colour<0||s->ground_projection<0||s->ground_modelview<0)return false;
    unsigned char pixels[64*64*3];
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
        bool check=((x>>3)+(y>>3))&1;unsigned char *p=pixels+(y*64+x)*3;
        p[0]=check?60+x*2:235;p[1]=check?90+y*2:238;p[2]=check?200:245;
    }
    glActiveTexture(GL_TEXTURE0);glGenTextures(1,&s->texture);
    if(!s->texture)return false;
    glBindTexture(GL_TEXTURE_2D,s->texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,64,64,0,GL_RGB,GL_UNSIGNED_BYTE,pixels);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    unsigned at=0;
    for(int y=-4;y<4;y++)for(int x=-4;x<4;x++){
        float alpha=((x+y)&1)?.55f:.28f,x0=x*.75f,y0=y*.75f;
        quad(s->ground+at,
            vertex(x0,y0,-1.35f,0,0,1,.30f,.58f,.95f,alpha,0,0),
            vertex(x0+.75f,y0,-1.35f,0,0,1,.30f,.58f,.95f,alpha,1,0),
            vertex(x0+.75f,y0+.75f,-1.35f,0,0,1,.30f,.58f,.95f,alpha,1,1),
            vertex(x0,y0+.75f,-1.35f,0,0,1,.30f,.58f,.95f,alpha,0,1));at+=6;
    }
    return scene_status("initialization");
}

static bool scene_draw(gl_scene_t *s,const scene_vertex_t *mesh,unsigned count,
                        bool lit,bool textured) {
    float modelview[16];glGetFloatv(GL_MODELVIEW,modelview);
    glUniformMatrix4fv(s->modelview,1,GL_FALSE,modelview);
    glUniform1f(s->lighting,lit);glUniform1f(s->textured,textured);
    GLint attrib[4]={s->position,s->normal,s->colour,s->uv};
    const float *data[4]={mesh->position,mesh->normal,mesh->colour,mesh->uv};
    const int size[4]={3,3,4,2};
    for(unsigned i=0;i<4;i++){
        glEnableVertexAttribArray(attrib[i]);
        glVertexAttribPointer(attrib[i],size[i],GL_FLOAT,GL_FALSE,sizeof(*mesh),data[i]);
    }
    glDrawArrays(GL_TRIANGLES,0,count);
    for(unsigned i=0;i<4;i++)glDisableVertexAttribArray(attrib[i]);
    return scene_status("draw");
}

static bool scene_frame(gl_scene_t *s,surface_t *canvas,int top,int w,int h,
                         float spin,float pitch,bool lit,bool textured) {
    glSetTarget(canvas);glUseProgram(s->program);
    glViewport(0,top,w,h);glDepthRange(0,1);glClearDepth(1);
    glDepthMask(GL_TRUE);glDisable(GL_BLEND);glDisable(GL_ALPHA_TEST);glDisable(GL_SCISSOR_TEST);
    glClearColor(.11f,.14f,.21f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);glFrontFace(GL_CCW);glShadeModel(GL_SMOOTH);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();gluPerspective(50,(double)w/h,.5,40);
    float projection[16];glGetFloatv(GL_PROJECTION,projection);
    glUniformMatrix4fv(s->projection,1,GL_FALSE,projection);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();gluLookAt(0,-5.4,2.5,0,0,0,0,0,1);
    float view[16];glGetFloatv(GL_MODELVIEW,view);
    float light[3];for(unsigned row=0;row<3;row++)
        light[row]=view[row]*2.5f+view[4+row]*-3+view[8+row]*4+view[12+row];
    glUniform3f(s->light,light[0],light[1],light[2]);
    // All application sampler state is explicit, including after an API switch.
    for(unsigned i=0;i<4;i++){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,0);}
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,s->texture);glUniform1i(s->image,0);
    glPushMatrix();glRotatef(spin,0,0,1);glRotatef(pitch,1,0,0);
    bool ok=scene_draw(s,s->mesh,s->count,lit,textured);glPopMatrix();
    if(ok){
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
        glUseProgram(s->ground_program);
        glUniformMatrix4fv(s->ground_projection,1,GL_FALSE,projection);
        glUniformMatrix4fv(s->ground_modelview,1,GL_FALSE,view);
        glEnableVertexAttribArray(s->ground_position);glEnableVertexAttribArray(s->ground_colour);
        glVertexAttribPointer(s->ground_position,3,GL_FLOAT,GL_FALSE,sizeof(scene_vertex_t),s->ground->position);
        glVertexAttribPointer(s->ground_colour,4,GL_FLOAT,GL_FALSE,sizeof(scene_vertex_t),s->ground->colour);
        glDrawArrays(GL_TRIANGLES,0,64*6);
        glDisableVertexAttribArray(s->ground_position);glDisableVertexAttribArray(s->ground_colour);
        ok=scene_status("ground draw");
    }
    glDepthMask(GL_TRUE);glDisable(GL_BLEND);glUseProgram(0);glFinish();
    return scene_status("frame retirement")&&ok;
}

bool gl_scene_render(gl_scene_t **state,surface_t *canvas,int top,int w,int h,
                     unsigned scene,float spin,float pitch,bool lit,bool textured) {
    if(!state||!canvas||!canvas->gpu||canvas->pixels||w<8||h<8||scene>=3||
       top<0||top>canvas->height||w>canvas->width||h>canvas->height-top||
       !__builtin_isfinite(spin)||!__builtin_isfinite(pitch))return false;
    if(!*state){
        gl_scene_t *s=calloc(1,sizeof(*s));if(!s)return false;
        if(!scene_init(s)){gl_scene_destroy(s);return false;}
        *state=s;
    }
    if(!scene_mesh(*state,scene))return false;
    return scene_frame(*state,canvas,top,w,h,spin,pitch,lit,textured);
}

void gl_scene_destroy(gl_scene_t *s) {
    if(!s)return;
    glFinish();glUseProgram(0);glSetTarget(NULL);
    glDeleteTextures(1,&s->texture);glDeleteProgram(s->program);
    glDeleteProgram(s->ground_program);
    free(s->mesh);free(s);
}
