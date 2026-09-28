/* app_gl_apis.c - the same cube, drawn through Vulkan and Direct3D.
 *
 * The point of this file is that the three graphics APIs are not decoration.
 * Each one here goes through its own object model - Vulkan's buffers, render
 * pass, pipeline and command buffer; Direct3D 9's fixed-function device;
 * Direct3D 11's device, input layout and immediate context - and each ends up in
 * the same rasteriser.  If any of the translation is wrong, the cube comes out
 * visibly wrong, so the demo is also the test.
 *
 * Each API's clip space differs from OpenGL's, and the projection matrices here
 * are built for the convention each one actually uses rather than being shared.
 */
#include "desktop.h"
#include "GL.h"
#include "vulkan.h"
#include "d3d.h"
#include "math.h"

/* One vertex, in the layout all three APIs are told about. */
typedef struct {
    float x, y, z;
    float r, g, b, a;
} api_vertex_t;
#include "vulkan_mesh.h"

/* A cube as 12 triangles, wound counter-clockwise seen from outside. */
#define CUBE_VERTS 36

static void build_cube(api_vertex_t *out) {
    static const float corner[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    };
    static const int face[6][4] = {
        {4,5,6,7}, {1,0,3,2}, {0,4,7,3}, {5,1,2,6}, {7,6,2,3}, {0,1,5,4},
    };
    static const float tint[6][3] = {
        {1.00f, 0.35f, 0.35f}, {0.35f, 1.00f, 0.45f}, {0.40f, 0.55f, 1.00f},
        {1.00f, 0.85f, 0.30f}, {1.00f, 0.45f, 0.95f}, {0.35f, 0.95f, 1.00f},
    };

    int n = 0;
    for (int f = 0; f < 6; f++) {
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) {
            const float *c = corner[face[f][order[k]]];
            out[n].x = c[0]; out[n].y = c[1]; out[n].z = c[2];
            out[n].r = tint[f][0]; out[n].g = tint[f][1]; out[n].b = tint[f][2];
            out[n].a = 1.0f;
            n++;
        }
    }
}

/* ------------------------------------------------------------------ matrices */

/* Column-major, the layout OpenGL and Vulkan both use for a raw float[16]. */
static void mat_identity(float *m) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_multiply(const float *a, const float *b, float *out) {
    float r[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = sum;
        }
    for (int i = 0; i < 16; i++) out[i] = r[i];
}

/* A perspective projection whose clip-space z runs from zero to one, which is
 * what Direct3D and Vulkan expect - not OpenGL's minus one to one. */
static void mat_perspective_zero_to_one(float fov_degrees, float aspect,
                                        float near_z, float far_z, float *m) {
    float f = 1.0f / tanf(radiansf(fov_degrees) * 0.5f);
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = far_z / (near_z - far_z);
    m[11] = -1.0f;
    m[14] = (near_z * far_z) / (near_z - far_z);
}

static void mat_rotate_y(float radians, float *m) {
    mat_identity(m);
    float c = cosf(radians), s = sinf(radians);
    m[0] = c;  m[8] = s;
    m[2] = -s; m[10] = c;
}

static void mat_rotate_x(float radians, float *m) {
    mat_identity(m);
    float c = cosf(radians), s = sinf(radians);
    m[5] = c;  m[9] = -s;
    m[6] = s;  m[10] = c;
}

static void mat_translate(float x, float y, float z, float *m) {
    mat_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

/* The model-view the demo uses: turn the cube, then push it away from the eye. */
static void build_modelview(float spin, float pitch, float *out) {
    float ry[16], rx[16], t[16], tmp[16];
    mat_rotate_y(spin, ry);
    mat_rotate_x(pitch, rx);
    mat_translate(0.0f, 0.0f, -5.0f, t);
    mat_multiply(rx, ry, tmp);
    mat_multiply(t, tmp, out);
}

/* ------------------------------------------------------------------- Vulkan */

typedef struct {
    bool ready;
    unsigned shape, detail, vertex_count;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    VkDeviceMemory memory;
    VkBuffer buffer;
    VkImage image;
    VkImageView view;
    VkRenderPass pass;
    VkFramebuffer framebuffer;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    surface_t *target;
    int width, height;
    char device_name[64];
} vk_demo_t;

static bool vk_setup(vk_demo_t *v, surface_t *target) {
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "KestrelOS 3D";
    app.apiVersion = (1u << 22);

    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &v->instance) != VK_SUCCESS) return false;

    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(v->instance, &n, &v->phys) != VK_SUCCESS || !n)
        return false;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(v->phys, &props);
    strlcpy(v->device_name, props.deviceName, sizeof v->device_name);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(v->phys, &dci, NULL, &v->device) != VK_SUCCESS) return false;
    vkGetDeviceQueue(v->device, 0, 0, &v->queue);

    /* The vertex buffer, filled through mapped memory as Vulkan requires. */
    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    v->vertex_count = vk_mesh_count(v->shape,v->detail);
    if (!v->vertex_count) return false;
    bci.size = sizeof(api_vertex_t) * v->vertex_count;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(v->device, &bci, NULL, &v->buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, v->buffer, &req);

    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    if (vkAllocateMemory(v->device, &mai, NULL, &v->memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(v->device, v->buffer, v->memory, 0) != VK_SUCCESS) return false;

    void *mapped = NULL;
    if (vkMapMemory(v->device, v->memory, 0, req.size, 0, &mapped) != VK_SUCCESS)
        return false;
    bool built = vk_mesh_build(mapped,v->vertex_count,v->shape,v->detail);
    vkUnmapMemory(v->device, v->memory);
    if (!built) return false;

    /* The image the render pass writes to, bound to the window's surface. */
    VkImageCreateInfo ii = {0};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent.width = (uint32_t)target->width;
    ii.extent.height = (uint32_t)target->height;
    ii.extent.depth = 1;
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    if (vkCreateImage(v->device, &ii, NULL, &v->image) != VK_SUCCESS) return false;
    if (vkBindImageToSurface(v->image, target) != VK_SUCCESS) return false;

    VkImageViewCreateInfo ivi = {0};
    ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivi.image = v->image;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = VK_FORMAT_B8G8R8A8_UNORM;
    if (vkCreateImageView(v->device, &ivi, NULL, &v->view) != VK_SUCCESS) return false;

    VkAttachmentDescription colour = {0};
    colour.format = VK_FORMAT_B8G8R8A8_UNORM;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkRenderPassCreateInfo rpi = {0};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &colour;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    if (vkCreateRenderPass(v->device, &rpi, NULL, &v->pass) != VK_SUCCESS) return false;

    VkFramebufferCreateInfo fbi = {0};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = v->pass;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &v->view;
    fbi.width = (uint32_t)target->width;
    fbi.height = (uint32_t)target->height;
    fbi.layers = 1;
    if (vkCreateFramebuffer(v->device, &fbi, NULL, &v->framebuffer) != VK_SUCCESS)
        return false;

    /* The pipeline.  Its vertex input description is what says which part of a
     * vertex is the position and which is the colour. */
    VkVertexInputBindingDescription binding = { 0, sizeof(api_vertex_t),
                                                VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(api_vertex_t, x) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(api_vertex_t, r) },
    };

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkPipelineLayoutCreateInfo pli = {0};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(v->device, &pli, NULL, &v->layout) != VK_SUCCESS)
        return false;

    VkGraphicsPipelineCreateInfo gpi = {0};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pRasterizationState = &rs;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.layout = v->layout;
    gpi.renderPass = v->pass;
    if (vkCreateGraphicsPipelines(v->device, NULL, 1, &gpi, NULL, &v->pipeline)
        != VK_SUCCESS) return false;

    VkCommandPoolCreateInfo cpi = {0};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    if (vkCreateCommandPool(v->device, &cpi, NULL, &v->pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo cbi = {0};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = v->pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(v->device, &cbi, &v->cmd) != VK_SUCCESS) return false;

    v->width = target->width;
    v->height = target->height;
    v->target = target;
    v->ready = true;
    return true;
}

static void vk_teardown(vk_demo_t *v) {
    /* The bridge consumes command/vertex data synchronously. Handles created
     * before a setup failure still belong to us even when ready is false.
     * Images borrow the canvas; teardown must never free that surface. */
    if (v->cmd) vkFreeCommandBuffers(v->device, v->pool, 1, &v->cmd);
    if (v->pool) vkDestroyCommandPool(v->device, v->pool, NULL);
    if (v->pipeline) vkDestroyPipeline(v->device, v->pipeline, NULL);
    if (v->layout) vkDestroyPipelineLayout(v->device, v->layout, NULL);
    if (v->framebuffer) vkDestroyFramebuffer(v->device, v->framebuffer, NULL);
    if (v->pass) vkDestroyRenderPass(v->device, v->pass, NULL);
    if (v->view) vkDestroyImageView(v->device, v->view, NULL);
    if (v->image) vkDestroyImage(v->device, v->image, NULL);
    if (v->buffer) vkDestroyBuffer(v->device, v->buffer, NULL);
    if (v->memory) vkFreeMemory(v->device, v->memory, NULL);
    if (v->device) vkDestroyDevice(v->device, NULL);
    if (v->instance) vkDestroyInstance(v->instance, NULL);
    memset(v, 0, sizeof *v);
}

void api_destroy_vulkan(void *state) {
    if (state) vk_teardown(state);
}

bool api_config_vulkan(void *state,unsigned shape,unsigned detail) {
    if (!state || !vk_mesh_count(shape,detail)) return false;
    vk_demo_t *v=state;
    if (v->shape==shape && v->detail==detail) return true;
    /* Submission is synchronous: previous buffers have retired at this point.
     * Release obsolete geometry before allocating its bounded replacement. */
    vk_teardown(v);v->shape=shape;v->detail=detail;
    return true;
}

bool api_draw_vulkan(void *state, surface_t *target, int top, int w, int h,
                     float spin, float pitch) {
    vk_demo_t *v = state;

    if (v->ready && (v->target != target || v->width != target->width ||
                     v->height != target->height)) {
        unsigned shape=v->shape,detail=v->detail;
        vk_teardown(v);v->shape=shape;v->detail=detail;
    }
    if (!v->ready && !vk_setup(v, target)) { vk_teardown(v); return false; }

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(v->cmd, &begin) != VK_SUCCESS) return false;

    VkClearValue clear;
    clear.color.float32[0] = 0.11f; clear.color.float32[1] = 0.14f;
    clear.color.float32[2] = 0.21f; clear.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rp = {0};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = v->pass;
    rp.framebuffer = v->framebuffer;
    rp.renderArea.offset.x = 0;
    rp.renderArea.offset.y = top;
    rp.renderArea.extent.width = (uint32_t)w;
    rp.renderArea.extent.height = (uint32_t)h;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(v->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(v->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);

    VkViewport vp = { 0.0f, (float)top, (float)w, (float)h, 0.0f, 1.0f };
    vkCmdSetViewport(v->cmd, 0, 1, &vp);

    /* With no vertex shader, the transform is handed over directly.  The
     * projection uses Vulkan's zero-to-one depth range. */
    float proj[16], mv[16], mvp[16];
    mat_perspective_zero_to_one(50.0f, (float)w / (float)h, 0.5f, 40.0f, proj);
    /* Vulkan's clip space has y pointing down, so a projection built the usual
     * way puts the scene upside down.  Negating the y scale is what every
     * Vulkan program does about it, and it is done here rather than hidden
     * inside the driver so the convention stays visible. */
    proj[5] = -proj[5];
    build_modelview(spin, pitch, mv);
    mat_multiply(proj, mv, mvp);
    vkCmdSetTransformKESTREL(v->cmd, mvp);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(v->cmd, 0, 1, &v->buffer, &offset);
    vkCmdDraw(v->cmd, v->vertex_count, 1, 0, 0);
    vkCmdEndRenderPass(v->cmd);
    if (vkEndCommandBuffer(v->cmd) != VK_SUCCESS) return false;

    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &v->cmd;
    if (vkQueueSubmit(v->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    return vkQueueWaitIdle(v->queue) == VK_SUCCESS;
}

/* -------------------------------------------------------------- Direct3D 9 */

typedef struct {
    bool ready;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *device;
    api_vertex_t cube[CUBE_VERTS];
    surface_t *target;
} d3d9_demo_t;

/* Direct3D's own vertex layout: position then a packed colour. */
typedef struct { float x, y, z; D3DCOLOR colour; } d3d9_vertex_t;

void api_destroy_d3d9(void *state) {
    d3d9_demo_t *d = state;
    if (!d) return;
    if (d->device) d->device->lpVtbl->Release(d->device);
    if (d->d3d) d->d3d->lpVtbl->Release(d->d3d);
    memset(d, 0, sizeof *d);
}

bool api_draw_d3d9(void *state, surface_t *target, int top, int w, int h,
                   float spin, float pitch) {
    d3d9_demo_t *d = state;

    if (!d->ready) {
        d->d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d->d3d) return false;
        if (FAILED(d->d3d->lpVtbl->CreateDevice(d->d3d, 0, 1, NULL, 0, NULL,
                                                &d->device))) {
            api_destroy_d3d9(d);
            return false;
        }
        build_cube(d->cube);
        d->ready = true;
    }

    if (d->target != target) {
        if (FAILED(D3D9SetTargetKESTREL(d->device, target))) return false;
        d->target = target;
    }

    IDirect3DDevice9 *dev = d->device;
    const IDirect3DDevice9Vtbl *vt = dev->lpVtbl;

    if (FAILED(vt->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
              D3DCOLOR_XRGB(28, 36, 54), 1.0f, 0))) return false;
    if (FAILED(vt->BeginScene(dev))) return false;

    D3DVIEWPORT9 vp = { 0, top, (UINT)w, (UINT)h, 0.0f, 1.0f };
    vt->SetViewport(dev, &vp);

    vt->SetRenderState(dev, D3DRS_ZENABLE, 1);
    vt->SetRenderState(dev, D3DRS_ZWRITEENABLE, 1);
    vt->SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESS);
    vt->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_CW);
    vt->SetRenderState(dev, D3DRS_LIGHTING, 0);
    vt->SetRenderState(dev, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);

    /* Direct3D matrices are row-major, so each one is the transpose of the
     * column-major form the other two APIs take. */
    float proj[16], mv[16];
    mat_perspective_zero_to_one(50.0f, (float)w / (float)h, 0.5f, 40.0f, proj);
    build_modelview(spin, pitch, mv);

    D3DMATRIX dproj, dview, dworld;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            dproj.m[row][col] = proj[col * 4 + row];
            dview.m[row][col] = mv[col * 4 + row];
            dworld.m[row][col] = (row == col) ? 1.0f : 0.0f;
        }

    vt->SetTransform(dev, D3DTS_PROJECTION, &dproj);
    vt->SetTransform(dev, D3DTS_VIEW, &dview);
    vt->SetTransform(dev, D3DTS_WORLD, &dworld);

    vt->SetFVF(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);

    d3d9_vertex_t verts[CUBE_VERTS];
    for (int i = 0; i < CUBE_VERTS; i++) {
        verts[i].x = d->cube[i].x;
        verts[i].y = d->cube[i].y;
        verts[i].z = d->cube[i].z;
        verts[i].colour = D3DCOLOR_ARGB(255,
            (int)(d->cube[i].r * 255.0f), (int)(d->cube[i].g * 255.0f),
            (int)(d->cube[i].b * 255.0f));
    }

    HRESULT drawn=vt->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, CUBE_VERTS / 3, verts,
                                     sizeof(d3d9_vertex_t));
    HRESULT ended=vt->EndScene(dev);
    if (FAILED(drawn) || FAILED(ended)) return false;
    return !FAILED(vt->Present(dev, NULL, NULL, NULL, NULL));
}

/* ------------------------------------------------------------- Direct3D 11 */

typedef struct {
    bool ready;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Buffer *vertices;
    ID3D11InputLayout *layout;
    ID3D11RenderTargetView *rtv;
    surface_t *target;
    UINT feature_level;
} d3d11_demo_t;

void api_destroy_d3d11(void *state) {
    d3d11_demo_t *d = state;
    if (!d) return;
    /* Context bindings are borrowed pointers in this bridge. Release the
     * context before its buffers/views so it cannot retain dangling bindings. */
    if (d->context) d->context->lpVtbl->Release(d->context);
    if (d->rtv) D3D11ReleaseRenderTargetViewKESTREL(d->rtv);
    if (d->layout) D3D11ReleaseInputLayoutKESTREL(d->layout);
    if (d->vertices) D3D11ReleaseBufferKESTREL(d->vertices);
    if (d->device) d->device->lpVtbl->Release(d->device);
    memset(d, 0, sizeof *d);
}

bool api_draw_d3d11(void *state, surface_t *target, int top, int w, int h,
                    float spin, float pitch) {
    d3d11_demo_t *d = state;

    if (!d->ready) {
        if (FAILED(D3D11CreateDevice(NULL, 0, NULL, 0, NULL, 0, 7,
                                     &d->device, &d->feature_level, &d->context))) {
            api_destroy_d3d11(d);
            return false;
        }

        api_vertex_t cube[CUBE_VERTS];
        build_cube(cube);

        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = sizeof cube;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init = { cube, 0, 0 };
        if (FAILED(d->device->lpVtbl->CreateBuffer(d->device, &bd, &init, &d->vertices))) {
            api_destroy_d3d11(d);
            return false;
        }

        /* The input layout stands in for the vertex shader: it is what names
         * which part of a vertex is the position and which is the colour. */
        D3D11_INPUT_ELEMENT_DESC elements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  0, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, 0, 0 },
        };
        if (FAILED(d->device->lpVtbl->CreateInputLayout(d->device, elements, 2,
                                                        NULL, 0, &d->layout))) {
            api_destroy_d3d11(d);
            return false;
        }
        d->ready = true;
    }

    if (d->target != target) {
        d->context->lpVtbl->OMSetRenderTargets(d->context, 0, NULL, NULL);
        if (d->rtv) D3D11ReleaseRenderTargetViewKESTREL(d->rtv);
        d->rtv = NULL;
        d->target = NULL;
        if (FAILED(d->device->lpVtbl->CreateRenderTargetViewFromSurfaceKESTREL(
                d->device, target, &d->rtv)))
            return false;
        d->target = target;
    }

    ID3D11DeviceContext *ctx = d->context;
    const ID3D11DeviceContextVtbl *vt = ctx->lpVtbl;

    vt->OMSetRenderTargets(ctx, 1, &d->rtv, NULL);

    const FLOAT clear[4] = { 0.11f, 0.14f, 0.21f, 1.0f };
    vt->ClearRenderTargetView(ctx, d->rtv, clear);

    D3D11_VIEWPORT vp = { 0.0f, (FLOAT)top, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f };
    vt->RSSetViewports(ctx, 1, &vp);

    vt->IASetInputLayout(ctx, d->layout);
    UINT stride = sizeof(api_vertex_t), offset = 0;
    vt->IASetVertexBuffers(ctx, 0, 1, &d->vertices, &stride, &offset);
    vt->IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    float proj[16], mv[16], mvp[16];
    mat_perspective_zero_to_one(50.0f, (float)w / (float)h, 0.5f, 40.0f, proj);
    build_modelview(spin, pitch, mv);
    mat_multiply(proj, mv, mvp);
    vt->SetTransformKESTREL(ctx, mvp);

    /* Depth and culling come from Direct3D 11's default rasteriser state, which
     * OMSetRenderTargets applies; a program wanting something else would create
     * the state objects for it. */
    vt->Draw(ctx, CUBE_VERTS, 0);
    return true;
}

/* ------------------------------------------------------------------- sizes */

size_t api_state_size(void) {
    size_t a = sizeof(vk_demo_t), b = sizeof(d3d9_demo_t), c = sizeof(d3d11_demo_t);
    size_t m = a > b ? a : b;
    return m > c ? m : c;
}

const char *api_vulkan_device_name(void *state) {
    vk_demo_t *v = state;
    return v->ready ? v->device_name : "";
}
