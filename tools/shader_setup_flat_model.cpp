/* Source-level flat-varying regression model. Include the production CUDA
 * setup control flow through its existing host-only adapter. Compile/link only
 * during current work; no claim of GPU execution or passing runtime assertions. */
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include "shader_setup_host_model.cpp"

static_assert(sizeof(kshs_config_t)==304,"unchanged config ABI");
static_assert(offsetof(kshs_config_t,flat_varying_mask)==84,"former reserved1 offset");
static_assert(offsetof(kshs_submission_t,setup)+offsetof(kshs_config_t,flat_varying_mask)==124,
              "unchanged submission field position");
static_assert(sizeof(kshs_submission_t)==56984,"unchanged submission ABI");

static kshs_submission_t submission;
static float registers[6*SR_REGISTERS*4];
static ksh_status_t vertex_status[6];
static kshr_command_t output[KSHS_OUTPUT_TRIANGLES];

static void set_register(unsigned reg,unsigned component,unsigned lane,float value) {
    registers[(reg*4u+component)*submission.setup.lanes+lane]=value;
}
static float flat_value(unsigned component) { return 0.75f+0.03125f*component; }
static void reset(void) {
    std::memset(&submission,0,sizeof submission);std::memset(registers,0,sizeof registers);
    std::memset(vertex_status,0,sizeof vertex_status);
    auto &c=submission.setup;
    c.version=KSHS_ABI;c.lanes=6;c.first_vertex=3;c.triangle_count=1;c.varying_count=2;
    c.framebuffer_width=c.framebuffer_height=128;c.viewport_w=c.viewport_h=128;
    c.clip_w=c.clip_h=128;c.depth_far=1;c.flat_varying_mask=1;
    /* The original third vertex lies above the clip plane. Flat values must
     * survive its removal instead of choosing a new polygon/fan vertex. */
    const float x[3]={-0.5f,0.5f,0.0f},y[3]={-0.5f,-0.5f,2.0f};
    for(unsigned lane=0;lane<c.lanes;lane++)vertex_status[lane]={KSH_COMPLETE,1};
    for(unsigned v=0;v<3;v++) {
        unsigned lane=c.first_vertex+v;
        set_register(SR_POSITION,0,lane,x[v]);set_register(SR_POSITION,1,lane,y[v]);
        set_register(SR_POSITION,2,lane,0);set_register(SR_POSITION,3,lane,1);
        for(unsigned k=0;k<4;k++) {
            set_register(SR_VARYING, k,lane,v==2?flat_value(k):0.125f+v*0.25f);
            set_register(SR_VARYING+1,k,lane,0.125f+v*0.375f);
        }
    }
}

int main(void) {
    reset();
    auto config=submission.setup;
    for(unsigned active=0;active<=8;active++)for(unsigned mask=0;mask<512;mask++) {
        config.varying_count=active;config.flat_varying_mask=mask;
        assert(setup_valid(config)==((mask&~((1u<<active)-1u))==0));
    }
    config=submission.setup;config.varying_count=UINT_MAX;
    assert(!setup_valid(config)); /* short-circuit prevents an undefined shift */
    config=submission.setup;config.flat_varying_mask=0x80000000u;assert(!setup_valid(config));
    config=submission.setup;config.first_vertex=4;assert(!setup_valid(config));
    config=submission.setup;config.triangle_count=2;assert(!setup_valid(config));
    config=submission.setup;config.lanes=KSH_MAX_LANES+1;assert(!setup_valid(config));

    kshs_storage_layout_t layout;assert(kshs_storage_layout(1,&layout));
    void *workspace=std::malloc((size_t)layout.bytes);assert(workspace);
    unsigned count=0;
    assert(test_model_setup(&submission,registers,vertex_status,workspace,output,KSHS_OUTPUT_TRIANGLES,&count));
    assert(count==2);bool varying_remains_smooth=false;
    for(unsigned i=0;i<count;i++)for(unsigned v=0;v<3;v++)for(unsigned k=0;k<4;k++) {
        float iw=output[i].raster.v[v].inv_w;
        assert(std::fabs(output[i].varying[v][0][k]/iw-flat_value(k))<1e-6f);
        if(std::fabs(output[i].varying[v][1][k]/iw-0.875f)>1e-6f)varying_remains_smooth=true;
    }
    assert(varying_remains_smooth);

    reset();submission.setup.flat_varying_mask=0;
    assert(test_model_setup(&submission,registers,vertex_status,workspace,output,KSHS_OUTPUT_TRIANGLES,&count));
    bool smooth_zero_mask=false;
    for(unsigned i=0;i<count;i++)for(unsigned v=0;v<3;v++)
        if(std::fabs(output[i].varying[v][0][0]/output[i].raster.v[v].inv_w-flat_value(0))>1e-6f)
            smooth_zero_mask=true;
    assert(smooth_zero_mask);

    /* Flat replacement must not hide a nonfinite original input or failed
     * vertex invocation, including an unused lane checked by primitive zero. */
    for(unsigned v=0;v<3;v++) {
        reset();set_register(SR_VARYING,0,submission.setup.first_vertex+v,std::numeric_limits<float>::quiet_NaN());
        assert(!test_model_setup(&submission,registers,vertex_status,workspace,output,KSHS_OUTPUT_TRIANGLES,&count));
        assert(count==0);
    }
    for(unsigned lane=0;lane<6;lane++) {
        reset();vertex_status[lane].result=KSH_PENDING;
        assert(!test_model_setup(&submission,registers,vertex_status,workspace,output,KSHS_OUTPUT_TRIANGLES,&count));
        assert(count==0);
    }
    std::free(workspace);
    return 0;
}
