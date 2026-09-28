/* gfxtest - render the same scene through every graphics API and compare.
 *
 * The three API layers all end at one rasteriser, so a cube drawn through
 * OpenGL, Vulkan, Direct3D 9 and Direct3D 11 at the same orientation must come
 * out as the same pixels.  Where it does not, the difference is in the
 * translation - a transposed matrix, an inverted winding, the wrong clip-space
 * convention - and those are exactly the mistakes that are hard to see by eye
 * on a spinning model and obvious in a pixel count.
 *
 * Everything renders off-screen, so this runs on the text console and reports a
 * number rather than needing someone to look at it.
 */
#include "kestrel.h"
#include "gui.h"
#include "GL.h"
#include "vulkan.h"
#include "d3d.h"
#include "math.h"

#define W 240
#define H 180

/* A fixed orientation: no animation, so every API is asked for the same frame. */
static const float SPIN  = 0.6f;
static const float PITCH = -0.35f;

typedef struct { float x, y, z, r, g, b, a; } vertex_t;
#define CUBE_VERTS 36

static void build_cube(vertex_t *out) {
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
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };

    int n = 0;
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 6; k++) {
            const float *c = corner[face[f][order[k]]];
            out[n].x = c[0]; out[n].y = c[1]; out[n].z = c[2];
            out[n].r = tint[f][0]; out[n].g = tint[f][1]; out[n].b = tint[f][2];
            out[n].a = 1.0f;
            n++;
        }
}

/* ------------------------------------------------------------------ matrices */

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

/* Clip-space z from zero to one: what Direct3D and Vulkan use. */
static void perspective_01(float fov, float aspect, float n, float f, float *m) {
    float t = 1.0f / tanf(radiansf(fov) * 0.5f);
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  = t / aspect;
    m[5]  = t;
    m[10] = f / (n - f);
    m[11] = -1.0f;
    m[14] = (n * f) / (n - f);
}

static void modelview(float *out) {
    float ry[16], rx[16], t[16], tmp[16];
    mat_identity(ry);
    float c = cosf(SPIN), s = sinf(SPIN);
    ry[0] = c; ry[8] = s; ry[2] = -s; ry[10] = c;

    mat_identity(rx);
    c = cosf(PITCH); s = sinf(PITCH);
    rx[5] = c; rx[9] = -s; rx[6] = s; rx[10] = c;

    mat_identity(t);
    t[14] = -5.0f;

    mat_multiply(rx, ry, tmp);
    mat_multiply(t, tmp, out);
}

/* ------------------------------------------------------------------- OpenGL */

static void draw_gl(surface_t *s, const vertex_t *cube) {
    glSetTarget(s);
    glViewport(0, 0, W, H);
    glClearColor(0.11f, 0.14f, 0.21f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glShadeModel(GL_SMOOTH);

    /* OpenGL's own projection, with its minus-one-to-one depth. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(50.0, (double)W / (double)H, 0.5, 40.0);

    float mv[16];
    modelview(mv);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(mv);

    glBegin(GL_TRIANGLES);
    for (int i = 0; i < CUBE_VERTS; i++) {
        glColor4f(cube[i].r, cube[i].g, cube[i].b, cube[i].a);
        glVertex3f(cube[i].x, cube[i].y, cube[i].z);
    }
    glEnd();
}

/* ------------------------------------------------------------------- Vulkan */

static bool draw_vulkan(surface_t *s, const vertex_t *cube) {
    VkInstance instance;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) return false;

    VkPhysicalDevice phys;
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(instance, &n, &phys) != VK_SUCCESS) return false;

    VkDevice device;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    if (vkCreateDevice(phys, &dci, NULL, &device) != VK_SUCCESS) return false;

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkBuffer buffer;
    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(vertex_t) * CUBE_VERTS;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(device, &bci, NULL, &buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);

    VkDeviceMemory memory;
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    if (vkAllocateMemory(device, &mai, NULL, &memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, buffer, memory, 0);

    void *mapped;
    vkMapMemory(device, memory, 0, req.size, 0, &mapped);
    memcpy(mapped, cube, sizeof(vertex_t) * CUBE_VERTS);
    vkUnmapMemory(device, memory);

    VkImage image;
    VkImageCreateInfo ii = {0};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent.width = W; ii.extent.height = H; ii.extent.depth = 1;
    vkCreateImage(device, &ii, NULL, &image);
    vkBindImageToSurface(image, s);

    VkImageView view;
    VkImageViewCreateInfo ivi = {0};
    ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivi.image = image;
    vkCreateImageView(device, &ivi, NULL, &view);

    VkAttachmentDescription att = {0};
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkRenderPass pass;
    VkRenderPassCreateInfo rpi = {0};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1;    rpi.pSubpasses = &sub;
    vkCreateRenderPass(device, &rpi, NULL, &pass);

    VkFramebuffer fb;
    VkFramebufferCreateInfo fbi = {0};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = pass;
    fbi.attachmentCount = 1; fbi.pAttachments = &view;
    fbi.width = W; fbi.height = H; fbi.layers = 1;
    vkCreateFramebuffer(device, &fbi, NULL, &fb);

    VkVertexInputBindingDescription binding = { 0, sizeof(vertex_t),
                                                VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(vertex_t, x) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vertex_t, r) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipeline pipeline;
    VkGraphicsPipelineCreateInfo gpi = {0};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pRasterizationState = &rs;
    gpi.pDepthStencilState = &ds;
    gpi.renderPass = pass;
    if (vkCreateGraphicsPipelines(device, NULL, 1, &gpi, NULL, &pipeline) != VK_SUCCESS)
        return false;

    VkCommandPool pool;
    VkCommandPoolCreateInfo cpi = {0};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    vkCreateCommandPool(device, &cpi, NULL, &pool);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbi = {0};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cbi, &cmd);

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    VkClearValue clear;
    clear.color.float32[0] = 0.11f; clear.color.float32[1] = 0.14f;
    clear.color.float32[2] = 0.21f; clear.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rp = {0};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = pass;
    rp.framebuffer = fb;
    rp.renderArea.extent.width = W;
    rp.renderArea.extent.height = H;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport vp = { 0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);

    float proj[16], mv[16], mvp[16];
    perspective_01(50.0f, (float)W / (float)H, 0.5f, 40.0f, proj);
    /* Vulkan's y points down, so the projection is negated as every Vulkan
     * program does; without it the image comes out mirrored. */
    proj[5] = -proj[5];
    modelview(mv);
    mat_multiply(proj, mv, mvp);
    vkCmdSetTransformKESTREL(cmd, mvp);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
    vkCmdDraw(cmd, CUBE_VERTS, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkDeviceWaitIdle(device);
    return true;
}

/* -------------------------------------------------------------- Direct3D 9 */

typedef struct { float x, y, z; D3DCOLOR colour; } d9_vertex_t;

static bool draw_d3d9(surface_t *s, const vertex_t *cube) {
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;

    IDirect3DDevice9 *dev = NULL;
    if (FAILED(d3d->lpVtbl->CreateDevice(d3d, 0, 1, NULL, 0, NULL, &dev))) return false;
    D3D9SetTargetKESTREL(dev, s);

    const IDirect3DDevice9Vtbl *vt = dev->lpVtbl;
    vt->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
              D3DCOLOR_XRGB(28, 36, 54), 1.0f, 0);
    vt->BeginScene(dev);

    D3DVIEWPORT9 vp = { 0, 0, W, H, 0.0f, 1.0f };
    vt->SetViewport(dev, &vp);
    vt->SetRenderState(dev, D3DRS_ZENABLE, 1);
    vt->SetRenderState(dev, D3DRS_ZWRITEENABLE, 1);
    vt->SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESS);
    vt->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_CW);
    vt->SetRenderState(dev, D3DRS_LIGHTING, 0);

    float proj[16], mv[16];
    perspective_01(50.0f, (float)W / (float)H, 0.5f, 40.0f, proj);
    modelview(mv);

    /* Direct3D matrices are row-major: the transpose of the other two. */
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

    d9_vertex_t verts[CUBE_VERTS];
    for (int i = 0; i < CUBE_VERTS; i++) {
        verts[i].x = cube[i].x; verts[i].y = cube[i].y; verts[i].z = cube[i].z;
        verts[i].colour = D3DCOLOR_ARGB(255, (int)(cube[i].r * 255.0f),
                                             (int)(cube[i].g * 255.0f),
                                             (int)(cube[i].b * 255.0f));
    }
    vt->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, CUBE_VERTS / 3, verts,
                        sizeof(d9_vertex_t));
    vt->EndScene(dev);
    vt->Release(dev);
    return true;
}

/* ------------------------------------------------------------- Direct3D 11 */

static bool draw_d3d11(surface_t *s, const vertex_t *cube) {
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *ctx = NULL;
    UINT level = 0;
    if (FAILED(D3D11CreateDevice(NULL, 0, NULL, 0, NULL, 0, 7, &device, &level, &ctx)))
        return false;

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = sizeof(vertex_t) * CUBE_VERTS;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { cube, 0, 0 };

    ID3D11Buffer *vb = NULL;
    if (FAILED(device->lpVtbl->CreateBuffer(device, &bd, &init, &vb))) return false;

    D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  0, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, 0, 0 },
    };
    ID3D11InputLayout *layout = NULL;
    if (FAILED(device->lpVtbl->CreateInputLayout(device, elements, 2, NULL, 0, &layout)))
        return false;

    ID3D11RenderTargetView *rtv = NULL;
    if (FAILED(device->lpVtbl->CreateRenderTargetViewFromSurfaceKESTREL(device, s, &rtv)))
        return false;

    const ID3D11DeviceContextVtbl *vt = ctx->lpVtbl;
    vt->OMSetRenderTargets(ctx, 1, &rtv, NULL);

    const FLOAT clear[4] = { 0.11f, 0.14f, 0.21f, 1.0f };
    vt->ClearRenderTargetView(ctx, rtv, clear);

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (FLOAT)W, (FLOAT)H, 0.0f, 1.0f };
    vt->RSSetViewports(ctx, 1, &vp);
    vt->IASetInputLayout(ctx, layout);

    UINT stride = sizeof(vertex_t), offset = 0;
    vt->IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    vt->IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    float proj[16], mv[16], mvp[16];
    perspective_01(50.0f, (float)W / (float)H, 0.5f, 40.0f, proj);
    modelview(mv);
    mat_multiply(proj, mv, mvp);
    vt->SetTransformKESTREL(ctx, mvp);

    vt->Draw(ctx, CUBE_VERTS, 0);

    D3D11ReleaseRenderTargetViewKESTREL(rtv);
    D3D11ReleaseInputLayoutKESTREL(layout);
    D3D11ReleaseBufferKESTREL(vb);
    vt->Release(ctx);
    return true;
}

/* ------------------------------------------------------------- comparison */

/* Count pixels that differ by more than a rounding step.  Exact equality is
 * too strict: the four paths reach the same colour through different float
 * arithmetic, so a channel can land one value apart. */
static int compare(const surface_t *a, const surface_t *b, int *worst) {
    int differing = 0;
    *worst = 0;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            colour_t pa = a->pixels[(size_t)y * a->stride + x];
            colour_t pb = b->pixels[(size_t)y * b->stride + x];
            int dr = (int)RGB_R(pa) - (int)RGB_R(pb);
            int dg = (int)RGB_G(pa) - (int)RGB_G(pb);
            int db = (int)RGB_B(pa) - (int)RGB_B(pb);
            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (db < 0) db = -db;
            int d = dr > dg ? dr : dg;
            if (db > d) d = db;
            if (d > *worst) *worst = d;
            if (d > 2) differing++;
        }
    }
    return differing;
}

/* How much of the frame the cube covers, as a guard against every API agreeing
 * because none of them drew anything. */
static int coverage(const surface_t *s) {
    colour_t background = s->pixels[0];
    int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (s->pixels[(size_t)y * s->stride + x] != background) n++;
    return n;
}


/* ------------------------------------------------------ the programmable path
 *
 * The tests above check that four fixed-function APIs agree.  These check the
 * thing everything shipped in the last twenty years actually uses: two small
 * programs, compiled from source at run time, that decide where a vertex lands
 * and what colour a pixel is.
 *
 * Each one is chosen for a mistake it would catch rather than for looking
 * impressive.  A shader stack that draws a pretty picture and gets swizzled
 * assignment backwards is wrong everywhere in a way nobody can see.
 */

static bool shader_ok(GLuint shader, const char *what) {
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status) return true;

    char log[256] = "";
    glGetShaderInfoLog(shader, sizeof log, NULL, log);
    printf("  %-28s FAILED to compile: %s\n", what, log);
    return false;
}

/* Compile, link and leave in use.  Returns 0 on any failure, having said why. */
static GLuint build_program(const char *vertex_source,
                            const char *fragment_source, const char *what) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertex_source, NULL);
    glCompileShader(vs);
    if (!shader_ok(vs, what)) { glDeleteShader(vs); return 0; }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragment_source, NULL);
    glCompileShader(fs);
    if (!shader_ok(fs, what)) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        printf("  %-28s FAILED to link: %s\n", what, log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glDeleteProgram(program);
        return 0;
    }

    /* Linked programs must outlive their source objects and recycled IDs. */
    glDetachShader(program, vs);
    glDetachShader(program, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

/* A full-surface quad in clip space, so a fragment shader's output can be read
 * back a pixel at a time and checked against arithmetic done here. */
static const float quad[] = {
    -1.0f, -1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
    -1.0f, -1.0f,   1.0f,  1.0f,  -1.0f,  1.0f,
};

static bool draw_quad(surface_t *target, GLuint program, colour_t *pixels,
                      bool native) {
    glSetTarget(target);
    glViewport(0, 0, target->width, target->height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(program);
    GLint position = glGetAttribLocation(program, "position");
    if (position < 0) position = 0;
    glEnableVertexAttribArray((GLuint)position);
    glVertexAttribPointer((GLuint)position, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUseProgram(0);
    glDisableVertexAttribArray((GLuint)position);
    glFinish();
    GLenum error = glGetError();
    if (error) {
        printf("  shader draw FAILED: GL error 0x%x\n", error);
        return false;
    }
    if (native) {
        if (!target->gpu || target->pixels ||
            !gui_gpu_colour_readback(target, pixels, 64, 64 * 64)) {
            printf("  shader draw FAILED: GPU-only target or VRAM readback lost\n");
            return false;
        }
    } else {
        for (int y = 0; y < 64; y++)
            memcpy(pixels + y * 64, target->pixels + y * target->stride,
                   64 * sizeof *pixels);
    }
    return true;
}

static bool near_colour(colour_t got, int r, int g, int b, int slack,
                        const char *what) {
    int dr = (int)RGB_R(got) - r, dg = (int)RGB_G(got) - g, db = (int)RGB_B(got) - b;
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    if (dr <= slack && dg <= slack && db <= slack) return true;

    printf("  %-28s FAILED: got %d,%d,%d, expected %d,%d,%d\n", what,
           RGB_R(got), RGB_G(got), RGB_B(got), r, g, b);
    return false;
}

/* Explicit native fixed-function proof. Both transforms and lighting execute
 * on the vertex VM; the old GPU rasterizer writes the final pixels. */
static int fixed_vertex_tests(void) {
    surface_t *s=surface_create(64,64);colour_t *pixels=malloc(64*64*sizeof(*pixels));
    if(!s || !pixels || !gui_gpu_attach(s)){
        printf("  fixed GPU FAILED: target/readback allocation\n");
        free(pixels);surface_destroy(s);return 1;
    }
    int failed=0;glSetTarget(s);glViewport(0,0,64,64);glUseProgram(0);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_TEXTURE_2D);
    glDisable(GL_COLOR_MATERIAL);glShadeModel(GL_SMOOTH);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    const float zero[4]={0,0,0,0},half[4]={.5f,.5f,.5f,1},position[4]={0,0,1,0};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT,zero);
    for(unsigned i=0;i<4;i++)glDisable(GL_LIGHT0+i);
    glLightfv(GL_LIGHT0,GL_POSITION,position);glLightfv(GL_LIGHT0,GL_AMBIENT,zero);
    glLightfv(GL_LIGHT0,GL_DIFFUSE,half);glLightfv(GL_LIGHT0,GL_SPECULAR,zero);
    glMaterialfv(GL_FRONT_AND_BACK,GL_EMISSION,zero);glMaterialfv(GL_FRONT_AND_BACK,GL_SPECULAR,zero);
    glMaterialf(GL_FRONT_AND_BACK,GL_SHININESS,0);
    glScalef(.5f,.5f,1);glEnable(GL_NORMALIZE);glNormal3f(0,0,4);
    unsigned before=0;glGetShaderStats(&before);
    for(unsigned lit=0;lit<2;lit++){
        if(lit){glEnable(GL_LIGHTING);glEnable(GL_LIGHT0);}else glDisable(GL_LIGHTING);
        glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        for(unsigned v=0;v<6;v++){
            float x=quad[v*2],y=quad[v*2+1];float colour[4]={(x+1)*.5f,(y+1)*.5f,.25f,1};
            if(lit){glMaterialfv(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE,colour);glColor3f(0,0,0);}
            else glColor4fv(colour);
            glVertex3f(x,y,0);
        }
        glEnd();glFinish();
        if(glGetError()!=GL_NO_ERROR || !s->gpu || s->pixels ||
           !gui_gpu_colour_readback(s,pixels,64,64*64)){
            printf("  fixed GPU FAILED: draw/readback %u\n",lit);failed=1;break;
        }
        for(unsigned y=0;y<64 && !failed;y++)for(unsigned x=0;x<64;x++){
            float r=0,g=0,b=0;
            if(x>=16&&x<48&&y>=16&&y<48){
                float scale=lit?.5f:1.f;r=((x+.5f-16)/32)*scale;
                g=((48-y-.5f)/32)*scale;b=.25f*scale;
            }
            if(!near_colour(pixels[y*64+x],(int)(r*255+.5f),(int)(g*255+.5f),(int)(b*255+.5f),2,
                            lit?"GPU fixed material/lighting":"GPU fixed transform/colour")){failed=1;break;}
        }
    }
    unsigned after=0;glGetShaderStats(&after);
    if(after<=before){printf("  fixed GPU FAILED: no retired vertex shader instructions\n");failed=1;}
    glDisable(GL_LIGHTING);glDisable(GL_NORMALIZE);glSetTarget(NULL);surface_destroy(s);free(pixels);
    if(!failed)printf("  native fixed GPU PASS: transforms + normal/lighting + per-vertex materials, 8192 VRAM pixels, %u shader instructions\n",after-before);
    return failed;
}

static int shader_tests(bool native) {
    int failures = 0;
    surface_t *s = surface_create(64, 64);
    if (!s) { printf("  no memory for the shader tests\n"); return 1; }
    colour_t *pixels = malloc(64 * 64 * sizeof *pixels);
    if (!pixels || (native && !gui_gpu_attach(s))) {
        printf("  FAILED: cannot create %s shader target/readback storage\n",
               native ? "GPU-only" : "CPU reference");
        free(pixels);
        surface_destroy(s);
        return 1;
    }
    printf("  backend: %s\n", native ? "native GPU vertex + fragment shaders; VRAM pixel checks"
                                      : "CPU reference (not hardware-acceleration evidence)");

    /* 1. A uniform reaches the fragment shader, arithmetic happens, and the
     *    result comes out the other end. */
    {
        GLuint p = build_program(
            "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
            "uniform vec3 tint;\n"
            "void main() { gl_FragColor = vec4(tint * 0.5, 1.0); }\n",
            "a uniform reaches a pixel");

        if (!p) failures++;
        else {
            glUseProgram(p);
            glUniform3f(glGetUniformLocation(p, "tint"), 1.0f, 0.4f, 0.8f);
            if (!draw_quad(s, p, pixels, native)) {
                glDeleteProgram(p); failures++; goto finished;
            }
            if (!near_colour(pixels[32 * 64 + 32], 128, 51, 102, 2,
                             "a uniform reaches a pixel"))
                failures++;
            else printf("  %-28s ok\n", "a uniform reaches a pixel");
            glDeleteProgram(p);
        }
    }

    /* 2. Assignment through a swizzle is a permutation.  "c.bgr = vec3(...)"
     *    must put the first component in blue, not in red.  A compiler that
     *    builds a write mask and forgets to permute passes every test where
     *    the swizzle happens to be in order - which is most of them. */
    {
        GLuint p = build_program(
            "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
            "void main() {\n"
            "  vec4 c = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "  c.bgr = vec3(1.0, 0.5, 0.25);\n"
            "  gl_FragColor = c;\n"
            "}\n",
            "a swizzled assignment permutes");

        if (!p) failures++;
        else {
            if (!draw_quad(s, p, pixels, native)) {
                glDeleteProgram(p); failures++; goto finished;
            }
            /* blue takes 1.0, green 0.5, red 0.25. */
            if (!near_colour(pixels[32 * 64 + 32], 64, 128, 255, 2,
                             "a swizzled assignment permutes"))
                failures++;
            else printf("  %-28s ok\n", "a swizzled assignment permutes");
            glDeleteProgram(p);
        }
    }

    /* 3. A varying interpolates across the triangle, and the two stages agree
     *    about which register carries which name even when they declare them
     *    in opposite orders.  That reordering is legal, common, and produces a
     *    picture that is quietly wrong if the linker skips it. */
    {
        GLuint p = build_program(
            "attribute vec2 position;\n"
            "varying vec3 tint;\n"
            "varying float shade;\n"
            "void main() {\n"
            "  gl_Position = vec4(position, 0.0, 1.0);\n"
            "  tint = vec3(1.0, 0.0, 0.0);\n"
            "  shade = position.x * 0.5 + 0.5;\n"
            "}\n",
            /* Declared the other way round on purpose. */
            "varying float shade;\n"
            "varying vec3 tint;\n"
            "void main() { gl_FragColor = vec4(tint * shade, 1.0); }\n",
            "varyings match up by name");

        if (!p) failures++;
        else {
            if (!draw_quad(s, p, pixels, native)) {
                glDeleteProgram(p); failures++; goto finished;
            }
            /* Red rising left to right: dark at the left edge, full at the
             * right. */
            colour_t left = pixels[32 * 64 + 2];
            colour_t right = pixels[32 * 64 + 61];
            bool ok = RGB_R(left) < 40 && RGB_R(right) > 215 &&
                      RGB_G(right) < 4 && RGB_B(right) < 4;
            if (!ok) {
                printf("  %-28s FAILED: left %d, right %d\n",
                       "varyings match up by name", RGB_R(left), RGB_R(right));
                failures++;
            } else {
                printf("  %-28s ok\n", "varyings match up by name");
            }
            glDeleteProgram(p);
        }
    }

    /* 4. discard throws the fragment away entirely - no colour and no depth -
     *    which is what makes it usable for cut-out shapes. */
    {
        GLuint p = build_program(
            "attribute vec2 position;\n"
            "varying vec2 uv;\n"
            "void main() {\n"
            "  uv = position;\n"
            "  gl_Position = vec4(position, 0.0, 1.0);\n"
            "}\n",
            "varying vec2 uv;\n"
            "void main() {\n"
            "  if (length(uv) > 0.5) discard;\n"
            "  gl_FragColor = vec4(0.2, 0.9, 0.3, 1.0);\n"
            "}\n",
            "discard leaves a hole");

        if (!p) failures++;
        else {
            if (!draw_quad(s, p, pixels, native)) {
                glDeleteProgram(p); failures++; goto finished;
            }
            colour_t middle = pixels[32 * 64 + 32];
            colour_t corner = pixels[2 * 64 + 2];
            bool ok = RGB_G(middle) > 200 && RGB_R(corner) == 0 &&
                      RGB_G(corner) == 0 && RGB_B(corner) == 0;
            if (!ok) {
                printf("  %-28s FAILED: middle %d,%d,%d corner %d,%d,%d\n",
                       "discard leaves a hole", RGB_R(middle), RGB_G(middle),
                       RGB_B(middle), RGB_R(corner), RGB_G(corner),
                       RGB_B(corner));
                failures++;
            } else {
                printf("  %-28s ok\n", "discard leaves a hole");
            }
            glDeleteProgram(p);
        }
    }

    /* 5. A fragment shader samples a texture through a sampler uniform, and
     *    which unit that sampler names is whatever glUniform1i said - set
     *    after the shader was compiled. */
    {
        static colour_t texels[4 * 4];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                texels[y * 4 + x] = ((x + y) & 1) ? RGB(255, 40, 40)
                                                  : RGB(40, 40, 255);

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, texels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glActiveTexture(GL_TEXTURE0);

        GLuint p = build_program(
            "attribute vec2 position;\n"
            "varying vec2 uv;\n"
            "void main() {\n"
            "  uv = position * 0.5 + 0.5;\n"
            "  gl_Position = vec4(position, 0.0, 1.0);\n"
            "}\n",
            "uniform sampler2D image;\n"
            "varying vec2 uv;\n"
            "void main() { gl_FragColor = texture2D(image, uv); }\n",
            "a shader samples a texture");

        if (!p) failures++;
        else {
            glUseProgram(p);
            glUniform1i(glGetUniformLocation(p, "image"), 1);
            if (!draw_quad(s, p, pixels, native)) {
                glDeleteProgram(p); glDeleteTextures(1, &texture);
                failures++; goto finished;
            }

            /* Whatever it read, it has to be one of the two colours in the
             * texture and not the white a sampler returns when it reaches
             * nothing. */
            int reds = 0, blues = 0, other = 0;
            for (int y = 4; y < 60; y += 7) {
                for (int x = 4; x < 60; x += 7) {
                    colour_t c = pixels[y * 64 + x];
                    if (RGB_R(c) > 200 && RGB_B(c) < 80) reds++;
                    else if (RGB_B(c) > 200 && RGB_R(c) < 80) blues++;
                    else other++;
                }
            }
            if (!reds || !blues || other) {
                printf("  %-28s FAILED: %d red, %d blue, %d neither\n",
                       "a shader samples a texture", reds, blues, other);
                failures++;
            } else {
                printf("  %-28s ok (%d red, %d blue)\n",
                       "a shader samples a texture", reds, blues);
            }
            glDeleteProgram(p);
        }
        glDeleteTextures(1, &texture);
    }

    /* 6. A vertex shader multiplies by a matrix uniform, which is what every
     *    vertex shader ever written does first. */
    {
        GLuint p = build_program(
            "uniform mat4 transform;\n"
            "attribute vec2 position;\n"
            "varying vec2 where;\n"
            "void main() {\n"
            "  vec4 clip = transform * vec4(position, 0.0, 1.0);\n"
            "  where = clip.xy;\n"
            "  gl_Position = clip;\n"
            "}\n",
            "varying vec2 where;\n"
            "void main() { gl_FragColor = vec4(where * 0.5 + 0.5, 0.0, 1.0); }\n",
            "a matrix uniform transforms");

        if (!p) failures++;
        else {
            /* Half size, so the quad covers only the middle of the surface and
             * the edges stay cleared - which is what says the matrix was
             * applied rather than ignored. */
            const float half[16] = {
                0.5f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.5f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
            };
            glUseProgram(p);
            glUniformMatrix4fv(glGetUniformLocation(p, "transform"), 1,
                               GL_FALSE, half);
            if (!draw_quad(s, p, pixels, native)) {
                glDeleteProgram(p); failures++; goto finished;
            }

            colour_t middle = pixels[32 * 64 + 32];
            colour_t corner = pixels[2 * 64 + 2];
            bool ok = RGB_R(middle) > 100 && RGB_R(middle) < 155 &&
                      RGB_R(corner) == 0 && RGB_G(corner) == 0;
            if (!ok) {
                printf("  %-28s FAILED: middle %d,%d corner %d,%d\n",
                       "a matrix uniform transforms", RGB_R(middle),
                       RGB_G(middle), RGB_R(corner), RGB_G(corner));
                failures++;
            } else {
                printf("  %-28s ok\n", "a matrix uniform transforms");
            }
            glDeleteProgram(p);
        }
    }

    /* 7. A loop, which is what a shader doing more than one light needs. */
    {
        GLuint p = build_program(
            "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
            "void main() {\n"
            "  float total = 0.0;\n"
            "  for (int i = 0; i < 4; i++) { total += 0.125; }\n"
            "  gl_FragColor = vec4(total, total, total, 1.0);\n"
            "}\n",
            "a loop runs the right number");

        if (!p) failures++;
        else {
            if (!draw_quad(s, p, pixels, native)) {
                glDeleteProgram(p); failures++; goto finished;
            }
            if (!near_colour(pixels[32 * 64 + 32], 128, 128, 128, 2,
                             "a loop runs the right number"))
                failures++;
            else printf("  %-28s ok\n", "a loop runs the right number");
            glDeleteProgram(p);
        }
    }

    /* 8. Source that is wrong has to be refused, with a reason.  A compiler
     *    that accepts anything produces pictures nobody can debug. */
    {
        GLuint bad = glCreateShader(GL_FRAGMENT_SHADER);
        const char *source = "void main() { gl_FragColor = notAThing * 2.0; }\n";
        glShaderSource(bad, 1, &source, NULL);
        glCompileShader(bad);

        GLint status = 1;
        glGetShaderiv(bad, GL_COMPILE_STATUS, &status);
        char log[256] = "";
        glGetShaderInfoLog(bad, sizeof log, NULL, log);

        if (status || !log[0]) {
            printf("  %-28s FAILED: broken source was accepted\n",
                   "broken source is refused");
            failures++;
        } else {
            printf("  %-28s ok (\"%s\")\n", "broken source is refused", log);
        }
        glDeleteShader(bad);

        /* And a vertex shader that never sets gl_Position draws nothing, so
         * saying so at compile time beats an empty screen. */
        GLuint silent = glCreateShader(GL_VERTEX_SHADER);
        const char *quiet = "attribute vec2 position;\n"
                            "void main() { vec2 unused = position; }\n";
        glShaderSource(silent, 1, &quiet, NULL);
        glCompileShader(silent);
        status = 1;
        glGetShaderiv(silent, GL_COMPILE_STATUS, &status);
        if (status) {
            printf("  %-28s FAILED: it was accepted\n",
                   "a shader that draws nothing");
            failures++;
        } else {
            printf("  %-28s ok\n", "a shader that draws nothing");
        }
        glDeleteShader(silent);
    }

    /* 9. Live programs retain independent executables and uniforms when the
     * source objects are deleted/recycled and the application switches back. */
    {
        const char *vertex = "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
        GLuint a = build_program(vertex,
            "uniform vec3 tint; void main() { gl_FragColor = vec4(tint * vec3(0.5, 1.0, 0.25), 1.0); }",
            "independent program A");
        GLuint b = build_program(vertex,
            "uniform vec3 tint; void main() { gl_FragColor = vec4(tint.bgr, 1.0); }",
            "independent program B");
        bool ok = a && b;
        if (ok) {
            glUseProgram(a); glUniform3f(glGetUniformLocation(a, "tint"), 1.0f, 0.25f, 1.0f);
            glUseProgram(b); glUniform3f(glGetUniformLocation(b, "tint"), 0.0f, 0.5f, 1.0f);
            for (int i = 0; i < 3 && ok; i++) {
                bool first = i != 1;
                ok = draw_quad(s, first ? a : b, pixels, native);
                if (ok) ok = near_colour(pixels[32 * 64 + 32],
                    first ? 128 : 255, first ? 64 : 128, first ? 64 : 0, 2,
                    "program code/uniform isolation");
            }
        }
        glUseProgram(0); glDeleteProgram(a); glDeleteProgram(b);
        if (!ok) failures++;
        else printf("  %-28s ok\n", "program code/uniform isolation");
    }

    /* Native immediate-mode shaders: attribute zero emits vertices, other
     * generic attributes are captured per vertex, and a triangle spanning
     * the 256-lane boundary must not disappear or use the last current value. */
    if(native) {
        GLuint p=build_program(
            "attribute vec4 position; attribute vec4 colour; varying vec4 value;"
            "void main(){gl_Position=vec4(position.xy*0.5,0.0,1.0);value=colour;}",
            "varying vec4 value; void main(){gl_FragColor=value;}",
            "GPU immediate shader stream");
        bool ok=p!=0;
        GLint pa=ok?glGetAttribLocation(p,"position"):-1;
        GLint ca=ok?glGetAttribLocation(p,"colour"):-1;
        if(pa!=0 || ca<1)ok=false;
        for(unsigned pass=0;pass<2 && ok;pass++) {
            glUseProgram(p);glSetTarget(s);glViewport(0,0,64,64);
            glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
            glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            /* Valid array descriptors but intentionally unreadable client
             * addresses: immediate mode must use current attributes instead. */
            glVertexAttribPointer((GLuint)pa,4,GL_FLOAT,GL_FALSE,0,(const void *)(uintptr_t)1);
            glVertexAttribPointer((GLuint)ca,4,GL_FLOAT,GL_FALSE,0,(const void *)(uintptr_t)1);
            glEnableVertexAttribArray((GLuint)pa);glEnableVertexAttribArray((GLuint)ca);
            unsigned quads=pass?43u:1u;
            glBegin(GL_TRIANGLES);
            for(unsigned q=0;q<quads;q++)for(unsigned v=0;v<6;v++) {
                float x=quad[v*2],y=quad[v*2+1];
                float r=(x+1)*.5f,g=(y+1)*.5f;
                if(pass){r=1-r;g=1-g;}
                if(q+1!=quads){r=0;g=0;}
                glVertexAttrib4f((GLuint)ca,r,g,.25f,1);
                if(v&1)glVertexAttrib4f(0,x*2,y*2,0,1);
                else glVertex4f(x*2,y*2,0,1);
            }
            glVertexAttrib4f((GLuint)ca,1,0,1,1); /* must not recolour queued vertices */
            glEnd();glFinish();
            glDisableVertexAttribArray((GLuint)pa);glDisableVertexAttribArray((GLuint)ca);
            ok=glGetError()==GL_NO_ERROR && s->gpu && !s->pixels &&
                gui_gpu_colour_readback(s,pixels,64,64*64);
            for(unsigned y=0;y<64 && ok;y++)for(unsigned x=0;x<64 && ok;x++) {
                float r=(x+.5f)/64, g=1-(y+.5f)/64;
                if(pass){r=1-r;g=1-g;}
                ok=near_colour(pixels[y*64+x],(int)(r*255+.5f),(int)(g*255+.5f),64,2,
                    pass?"GPU immediate batch boundary":"GPU immediate attributes");
            }
        }
        glUseProgram(0);glDeleteProgram(p);
        if(!ok){failures++;printf("  GPU immediate shader stream FAILED\n");}
        else printf("  GPU immediate shader stream ok: 2 complete 64x64 pixel comparisons\n");
    }

finished:
    glSetTarget(NULL); /* release fenced shader staging before destroying target */
    surface_destroy(s);
    free(pixels);

    unsigned instructions = 0;
    glGetShaderStats(&instructions);
    if (failures)
        printf("\ngfxtest: FAILED - %d shader check(s) did not pass\n", failures);
    else
        printf("\ngfxtest: %s shader pixel checks PASS; %u instructions executed\n",
               native ? "native GPU" : "CPU reference", instructions);
    return failures;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "gpu-shaders")) return shader_tests(true) ? 1 : 0;
    if (argc == 2 && !strcmp(argv[1], "gpu-fixed")) return fixed_vertex_tests() ? 1 : 0;
    if (argc != 1) {
        printf("usage: gfxtest [gpu-shaders|gpu-fixed]\n");
        return 1;
    }
    printf("gfxtest: the same cube through every graphics API\n\n");

    vertex_t cube[CUBE_VERTS];
    build_cube(cube);

    surface_t *ref = surface_create(W, H);
    surface_t *tmp = surface_create(W, H);
    if (!ref || !tmp) {
        printf("gfxtest: out of memory\n");
        return 1;
    }

    draw_gl(ref, cube);
    int total = W * H;
    int covered = coverage(ref);
    printf("  %-14s %5d of %d pixels drawn (%d%%)\n", "OpenGL", covered, total,
           covered * 100 / total);

    if (covered < total / 20) {
        printf("\ngfxtest: FAILED - OpenGL drew almost nothing\n");
        return 1;
    }

    struct {
        const char *name;
        bool (*draw)(surface_t *, const vertex_t *);
    } apis[] = {
        { "Vulkan",      draw_vulkan },
        { "Direct3D 9",  draw_d3d9 },
        { "Direct3D 11", draw_d3d11 },
    };

    int failures = 0;
    for (size_t i = 0; i < sizeof apis / sizeof apis[0]; i++) {
        memset(tmp->pixels, 0, (size_t)W * H * sizeof(colour_t));

        if (!apis[i].draw(tmp, cube)) {
            printf("  %-14s could not be set up\n", apis[i].name);
            failures++;
            continue;
        }

        int worst = 0;
        int differing = compare(ref, tmp, &worst);
        int drawn = coverage(tmp);
        int permille = differing * 1000 / total;

        printf("  %-14s %5d pixels drawn, %5d differ from OpenGL (%d.%d%%),"
               " worst channel %d\n",
               apis[i].name, drawn, differing, permille / 10, permille % 10, worst);

        /* A handful of edge pixels can land differently; a real mistake in the
         * translation moves a large fraction of the frame. */
        if (differing > total / 100) failures++;
    }

    surface_destroy(ref);
    surface_destroy(tmp);

    if (failures) {
        printf("\ngfxtest: FAILED - %d API(s) do not match OpenGL\n", failures);
        return 1;
    }
    printf("\ngfxtest: all APIs agree\n");
    

    /* And then the path everything shipped in the last twenty years
     * actually uses: two programs compiled from source at run time. */
    printf("\nshaders:\n");
    if (shader_tests(false)) return 1;

    return 0;
}
