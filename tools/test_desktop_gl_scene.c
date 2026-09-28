/* App source and native GL implementation are compiled into this harness.
 * The reference alone is allowed to call the CPU VM; native draws abort on it.
 * Its clip and screen-grid setup are aligned with the native raster contract;
 * the unchanged legacy float-edge raster is not a bit-exact coverage oracle.
 * CUDA is the submission boundary, not evidence of native Kestrel dispatch. */
void test_cuda_scene_finished(void);
void test_cuda_timing(uint64_t out[3]);
void test_cuda_counts(unsigned out[3]);
void test_cuda_uploads(uint64_t out[2]);
uint64_t test_wall_us(void);
static void scene_vertex_uploads(GLuint program,unsigned *planes,unsigned *spans){
    const sh_shader_t *shader=g_gl.program_object[program-1].program.vertex;
    bool accessed[SR_REGISTERS]={0};accessed[SR_POSITION]=true;
    for(unsigned r=SR_VARYING;r<SR_VARYING+SR_VARYING_N;r++)accessed[r]=true;
    for(int pc=0;pc<shader->count;pc++){
        const sh_instruction_t *in=&shader->code[pc];
        assert(in->dst<SR_REGISTERS);accessed[in->dst]=true;
        for(unsigned i=0;i<3;i++){assert(in->src[i]<SR_REGISTERS);accessed[in->src[i]]=true;}
        if(in->op==SH_MATMUL)for(unsigned i=1;i<4;i++){assert(in->src[0]+i<SR_REGISTERS);accessed[in->src[0]+i]=true;}
    }
    *planes=*spans=0;
    for(unsigned r=0;r<SR_REGISTERS;r++)if(accessed[r]){(*planes)++;if(!r||!accessed[r-1])(*spans)++;}
}
static void check_mesh(const gl_scene_t *s) {
    assert(s->count==(s->scene==0?36:s->scene==1?3072:3456));
    for(unsigned i=0;i<s->count;i++){
        const scene_vertex_t *v=s->mesh+i;
        for(unsigned j=0;j<12;j++)assert(isfinite(((const float*)v)[j]));
        float length=0;for(unsigned j=0;j<3;j++)length+=v->normal[j]*v->normal[j];
        assert(fabsf(length-1)<1e-5f);
    }
    for(unsigned i=0;i<s->count;i+=3){
        const scene_vertex_t *v=s->mesh+i;float a[3],b[3],cross[3];
        for(unsigned j=0;j<3;j++){a[j]=v[1].position[j]-v[0].position[j];b[j]=v[2].position[j]-v[0].position[j];}
        cross[0]=a[1]*b[2]-a[2]*b[1];cross[1]=a[2]*b[0]-a[0]*b[2];cross[2]=a[0]*b[1]-a[1]*b[0];
        float dot=0;for(unsigned j=0;j<3;j++)dot+=cross[j]*v->normal[j];
        assert(dot>0);
    }
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    puts("Reference: CPU shader/fill with full clipping and native 1/16-pixel grid; GPU sources unchanged");
    gl_scene_t *scene=NULL;
    surface_t *native=surface_create(96,94),*reference=surface_create(96,94);
    assert(gui_gpu_attach(native));
    assert(!gl_scene_render(&scene,reference,7,96,80,0,0,0,true,true)&&!scene);
    assert(!gl_scene_render(&scene,native,7,96,80,3,0,0,true,true)&&!scene);
    assert(!gl_scene_render(&scene,native,-1,96,80,0,0,0,true,true)&&!scene);
    assert(!gl_scene_render(&scene,native,7,97,80,0,0,0,true,true)&&!scene);
    assert(!gl_scene_render(&scene,native,7,96,88,0,0,0,true,true)&&!scene);
    assert(!gl_scene_render(&scene,native,7,96,80,0,NAN,0,true,true)&&!scene);
    colour_t pixels[96*94];unsigned compared=0,max_error=0;
    for(unsigned which=0;which<3;which++){
        const scene_vertex_t *cached=NULL;
        for(unsigned mode=0;mode<4;mode++){
            float spin=mode*37.0f+11.0f,pitch=-14.0f+mode*9.0f;
            bool lit=!!(mode&1),textured=!!(mode&2);
            gl_set_clip_depth_zero_to_one(true);gl_set_clip_y_down(true); // prior API state
            assert(gl_scene_render(&scene,native,7,96,80,which,spin,pitch,lit,textured));
            assert(!native->pixels&&g_gl.gpu_target&&!g_gl.depth);
            assert(!g_gl.clip_depth_zero_to_one&&!g_gl.clip_y_down&&!g_gl.bound_program);
            if(cached)assert(cached==scene->mesh);else cached=scene->mesh;
            check_mesh(scene);
            assert(gui_gpu_colour_readback(native,pixels,96,96*94));
            scene_reference=true;
            assert(scene_frame(scene,reference,7,96,80,spin,pitch,lit,textured));
            scene_reference=false;
            unsigned coverage=0,bad=0;
            for(unsigned y=0;y<94;y++)for(unsigned x=0;x<96;x++){
                unsigned i=y*96+x;
                if(y<7||y>=87){assert(!pixels[i]);continue;} // control bars untouched
                colour_t want=reference->pixels[i],got=pixels[i];
                coverage+=(want&0xffffffu)!=0x1c2436u;
                for(unsigned shift=0;shift<24;shift+=8){
                    unsigned a=(want>>shift)&255,b=(got>>shift)&255;
                    unsigned error=a>b?a-b:b-a;
                    if(error>max_error)max_error=error;
                    if(error>2){
                        if(bad<8)printf("mismatch %u,%u channel=%u want=%06x got=%06x error=%u\n",x,y,shift,want,got,error);
                        bad++;
                    }
                }
                compared++;
            }
            printf("scene=%u lit=%u texture=%u: coverage=%u bad channels=%u\n",which,lit,textured,coverage,bad);
            assert(coverage>96*80/8&&!bad);
        }
    }
    // Default desktop window size: verify actual tiling/depth/pitch and frame
    // content without shrinking the render target to make the test pass.
    {
        surface_t *large=surface_create(640,520),*cpu=surface_create(640,520);
        assert(gui_gpu_attach(large));colour_t *out=malloc(640*520*4);assert(out);
        uint64_t before[3],after[3];unsigned count_before[3],count_after[3];
        test_cuda_timing(before);test_cuda_counts(count_before);uint64_t wall=test_wall_us();
        assert(gl_scene_render(&scene,large,32,640,456,0,23,-14,true,true));
        wall=test_wall_us()-wall;test_cuda_timing(after);test_cuda_counts(count_after);
        assert(count_after[1]-count_before[1]<=7); // formerly 53 full-resolution submissions
        printf("640x520 CUDA adapter frame: wall=%llu us, GPU vertex=%llu raster=%llu clear=%llu us (not Kestrel timing)\n",
            (unsigned long long)wall,(unsigned long long)(after[0]-before[0]),
            (unsigned long long)(after[1]-before[1]),(unsigned long long)(after[2]-before[2]));
        assert(gui_gpu_colour_readback(large,out,640,640*520));
        scene_reference=true;assert(scene_frame(scene,cpu,32,640,456,23,-14,true,true));scene_reference=false;
        unsigned bad=0,coverage=0;
        for(unsigned y=0;y<520;y++)for(unsigned x=0;x<640;x++){
            unsigned i=y*640+x;if(y<32||y>=488){assert(!out[i]);continue;}
            coverage+=(out[i]&0xffffffu)!=0x1c2436u;
            for(unsigned k=0;k<24;k+=8){
                int error=(int)((out[i]>>k)&255)-(int)((cpu->pixels[i]>>k)&255);if(error<0)error=-error;
                if((unsigned)error>max_error)max_error=(unsigned)error;
                if(error>2)bad++;
            }compared++;
        }
        printf("640x520 frame: coverage=%u bad channels=%u\n",coverage,bad);
        assert(!bad&&coverage>640*456/8);
        colour_t *repeat=malloc(640*520*4);assert(repeat);
        uint64_t wall_samples[5],gpu_samples[5];
        for(unsigned sample=0;sample<5;sample++){
            uint64_t upload_before[2],upload_after[2];test_cuda_uploads(upload_before);
            test_cuda_counts(count_before);test_cuda_timing(before);wall=test_wall_us();
            assert(gl_scene_render(&scene,large,32,640,456,0,23,-14,true,true));
            wall_samples[sample]=test_wall_us()-wall;test_cuda_timing(after);test_cuda_counts(count_after);
            gpu_samples[sample]=(after[0]-before[0])+(after[1]-before[1])+(after[2]-before[2]);
            assert(count_after[1]-count_before[1]<=7);
            assert(count_after[0]-count_before[0]==2); // object plus ground, one vertex batch each
            test_cuda_uploads(upload_after);
            // Only bytecode-accessed vertex planes upload. Exact span/byte
            // counts also exclude a repeated immutable texture-atlas transfer.
            unsigned planes,spans,gplanes,gspans;scene_vertex_uploads(scene->program,&planes,&spans);
            scene_vertex_uploads(scene->ground_program,&gplanes,&gspans);
            assert(planes<SR_REGISTERS/2);
            assert(gplanes<planes);
            assert(upload_after[0]-upload_before[0]==spans+gspans);
            assert(upload_after[1]-upload_before[1]==(36u*planes+384u*gplanes)*16u);
            if(!sample)printf("vertex input upload: %u/200 planes in %u spans, %llu -> %llu bytes/frame\n",planes,spans,
                (unsigned long long)((36u+384u)*SR_REGISTERS*16u),(unsigned long long)(upload_after[1]-upload_before[1]));
            assert(gui_gpu_colour_readback(large,repeat,640,640*520));
            assert(!memcmp(out,repeat,640*520*4)); // repeated frames cannot accumulate stale alpha/depth
        }
        for(unsigned i=0;i<5;i++)for(unsigned j=i+1;j<5;j++){
            if(wall_samples[i]>wall_samples[j]){uint64_t t=wall_samples[i];wall_samples[i]=wall_samples[j];wall_samples[j]=t;}
            if(gpu_samples[i]>gpu_samples[j]){uint64_t t=gpu_samples[i];gpu_samples[i]=gpu_samples[j];gpu_samples[j]=t;}
        }
        printf("640x520 five-repeat CUDA adapter median: wall=%llu us [%llu..%llu], GPU kernels=%llu us; <=7 fragment calls/frame, no repeated texture uploads, bit-identical pixels (not Kestrel timing)\n",
            (unsigned long long)wall_samples[2],(unsigned long long)wall_samples[0],(unsigned long long)wall_samples[4],(unsigned long long)gpu_samples[2]);
        free(repeat);free(out);glSetTarget(NULL);surface_destroy(large);surface_destroy(cpu);
    }
    // Match the native log's large viewport. These are GPU-adapter measurements,
    // not native Kestrel timings. Exact pixels are independently checked above;
    // repeated identical large frames must remain bit-identical and nonempty.
    {
        surface_t *large=surface_create(1920,952);assert(gui_gpu_attach(large));
        colour_t *first=malloc(1920*952*4),*last=malloc(1920*952*4);assert(first&&last);
        for(unsigned which=0;which<3;which++){
            assert(gl_scene_render(&scene,large,32,1920,888,which,23,-14,true,true));
            assert(gui_gpu_colour_readback(large,first,1920,1920*952));
            uint64_t walls[5],kernels[5],bytes=0;unsigned calls=0,vertices=0,uploads=0;
            for(unsigned sample=0;sample<5;sample++){
                uint64_t before[3],after[3],up_before[2],up_after[2];unsigned cb[3],ca[3];
                test_cuda_timing(before);test_cuda_counts(cb);test_cuda_uploads(up_before);
                uint64_t start=test_wall_us();
                assert(gl_scene_render(&scene,large,32,1920,888,which,23,-14,true,true));
                walls[sample]=test_wall_us()-start;test_cuda_timing(after);test_cuda_counts(ca);test_cuda_uploads(up_after);
                kernels[sample]=after[0]-before[0]+after[1]-before[1]+after[2]-before[2];
                bytes=up_after[1]-up_before[1];calls=ca[1]-cb[1];
                vertices=ca[0]-cb[0];uploads=(unsigned)(up_after[0]-up_before[0]);
                unsigned planes,spans,gplanes,gspans;scene_vertex_uploads(scene->program,&planes,&spans);
                scene_vertex_uploads(scene->ground_program,&gplanes,&gspans);
                assert(vertices==2 && uploads==spans+gspans);
                assert(bytes==((uint64_t)scene->count*planes+384u*gplanes)*16u);
            }
            assert(gui_gpu_colour_readback(large,last,1920,1920*952));
            assert(!memcmp(first,last,1920*952*4));
            unsigned coverage=0;for(unsigned y=32;y<920;y++)for(unsigned x=0;x<1920;x++)
                coverage+=(last[y*1920+x]&0xffffffu)!=0x1c2436u;
            assert(coverage>1920*888/8);
            for(unsigned i=0;i<5;i++)for(unsigned j=i+1;j<5;j++){
                if(walls[i]>walls[j]){uint64_t t=walls[i];walls[i]=walls[j];walls[j]=t;}
                if(kernels[i]>kernels[j]){uint64_t t=kernels[i];kernels[i]=kernels[j];kernels[j]=t;}
            }
            printf("1920x888 %s five-repeat CUDA adapter median: wall=%llu us [%llu..%llu], GPU kernels=%llu us, input bytes=%llu, vertex calls=%u, input uploads=%u, fragment calls=%u, bit-identical repeated frame (not native Kestrel)\n",
                which==2?"terrain":which==1?"torus":"cube",(unsigned long long)walls[2],(unsigned long long)walls[0],(unsigned long long)walls[4],
                (unsigned long long)kernels[2],(unsigned long long)bytes,vertices,uploads,calls);
        }
        free(first);free(last);glSetTarget(NULL);surface_destroy(large);
    }
    // Actual CUDA pixels must observe image/sampler changes and recycled names;
    // identical output for a stale atlas is not a residency success.
    for(unsigned phase=0;phase<6;phase++){
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,scene->texture);
        unsigned char changed[64*64*4];
        for(unsigned i=0;i<64*64;i++){
            changed[i*4]=(unsigned char)(i*13+phase*39);
            changed[i*4+1]=(unsigned char)(i*7+phase*73);
            changed[i*4+2]=(unsigned char)(255-i*3);changed[i*4+3]=255;
        }
        if(phase==0)glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,changed);
        if(phase==1){
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        if(phase==2)glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,5,3,0,GL_RGBA,GL_UNSIGNED_BYTE,changed);
        if(phase==3){
            GLuint name=scene->texture;uint64_t version=g_gl.textures[name].content_version;
            for(unsigned unit=0;unit<4;unit++){glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,name);}
            glDeleteTextures(1,&name);for(unsigned unit=0;unit<4;unit++)assert(!g_gl.unit_texture[unit]);
            glGenTextures(1,&scene->texture);assert(scene->texture==name);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,scene->texture);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,5,3,0,GL_RGBA,GL_UNSIGNED_BYTE,changed);
            assert(g_gl.textures[name].content_version!=version);
        }
        if(phase==4)glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,5,3,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        if(phase==5){
            uint64_t version=g_gl.textures[scene->texture].content_version;
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,5,3,0,GL_RGBA,GL_FLOAT,changed);
            assert(glGetError()==GL_INVALID_ENUM&&g_gl.textures[scene->texture].content_version==version);
        }
        assert(gl_scene_render(&scene,native,7,96,80,0,17,-9,true,true));
        assert(gui_gpu_colour_readback(native,pixels,96,96*94));
        scene_reference=true;assert(scene_frame(scene,reference,7,96,80,17,-9,true,true));scene_reference=false;
        unsigned bad=0;
        for(unsigned y=7;y<87;y++)for(unsigned x=0;x<96;x++){
            unsigned i=y*96+x;
            for(unsigned k=0;k<24;k+=8){
                int error=(int)((pixels[i]>>k)&255)-(int)((reference->pixels[i]>>k)&255);if(error<0)error=-error;
                if((unsigned)error>max_error)max_error=(unsigned)error;if(error>2)bad++;
            }compared++;
        }
        printf("texture residency phase %u: bad channels=%u\n",phase,bad);assert(!bad);
    }
    gl_scene_destroy(scene);glSetTarget(NULL);
    surface_destroy(native);surface_destroy(reference);free(g_gl.depth);g_gl.depth=NULL;
    for(unsigned i=0;i<GL_MAX_SHADER_OBJECTS;i++)assert(!g_gl.shader_object[i].used);
    for(unsigned i=0;i<GL_MAX_PROGRAM_OBJECTS;i++)assert(!g_gl.program_object[i].used);
    for(unsigned i=0;i<GL_MAX_TEXTURES;i++)assert(!g_gl.textures[i].texels);
    test_cuda_scene_finished();
    printf("PASS 19 compared desktop frames / %u pixel comparisons (max channel error %u), plus 18 large GPU repeat frames; cube/torus/terrain, lighting/texturing, model rotation, blended depth-tested ground, mesh cache, control-bar guards, API-state reset, texture residency/update/name reuse, default/native-log window sizes and cleanup\n",compared,max_error);
}
