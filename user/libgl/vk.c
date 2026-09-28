/* vk.c - Vulkan on the software rasteriser.
 *
 * The object model is real: instances, devices, queues, memory, buffers,
 * render passes, pipelines and command buffers all exist, are validated, and
 * have the lifetimes Vulkan specifies.  Commands are recorded into a command
 * buffer and only take effect at vkQueueSubmit, which is the part programs
 * actually depend on being right.
 *
 * The pipeline is fixed-function.  A VkPipeline records its vertex input
 * layout, topology, cull and depth state; the vertex attributes named by that
 * layout - position, colour, texture coordinate, by location - are what the
 * rasteriser reads.  Shader modules are accepted and kept so that a program's
 * lifetimes are honoured, but there is no SPIR-V interpreter behind them and
 * this file does not pretend otherwise.
 */
#include "vulkan.h"
#include "glstate.h"
#include "../libc/math.h"

/* ------------------------------------------------------------- the objects */

struct VkInstance_T { uint32_t magic; };
struct VkPhysicalDevice_T { uint32_t magic; };
struct VkDevice_T { uint32_t magic; };
struct VkQueue_T { struct VkDevice_T *device; VkResult error; };
struct VkFence_T { int signalled; };
struct VkCommandPool_T { uint32_t magic; };
struct VkShaderModule_T { size_t code_size; };
struct VkPipelineLayout_T { uint32_t magic; };

struct VkDeviceMemory_T {
    void       *host;
    VkDeviceSize size;
    int          mapped;
};

struct VkBuffer_T {
    VkDeviceSize size;
    VkFlags      usage;
    struct VkDeviceMemory_T *memory;
    VkDeviceSize offset;
};

struct VkImage_T {
    uint32_t   width, height;
    VkFormat   format;
    surface_t *surface;          /* where it really lives */
};

struct VkImageView_T { struct VkImage_T *image; };

struct VkRenderPass_T {
    VkAttachmentLoadOp load_op;
    int has_depth;
};

struct VkFramebuffer_T {
    struct VkImageView_T *colour;
    uint32_t width, height;
};

#define VK_MAX_ATTRS 8

struct VkPipeline_T {
    VkPrimitiveTopology topology;
    VkFlags  cull_mode;
    VkFrontFace front_face;
    int      depth_test, depth_write;
    VkCompareOp depth_compare;
    int      blend;
    VkBlendFactor blend_src, blend_dst;

    uint32_t binding_stride;
    struct {
        uint32_t location, offset;
        VkFormat format;
        int      components;
    } attr[VK_MAX_ATTRS];
    uint32_t attr_count;

    VkViewport viewport;
    int        have_viewport;
};

/* One recorded command.  Keeping them as a list rather than executing on the
 * spot is what makes a command buffer replayable, which is the whole point of
 * the object. */
typedef enum {
    CMD_BEGIN_PASS, CMD_END_PASS, CMD_BIND_PIPELINE, CMD_SET_VIEWPORT,
    CMD_SET_SCISSOR, CMD_BIND_VERTEX, CMD_BIND_INDEX, CMD_DRAW,
    CMD_DRAW_INDEXED, CMD_SET_TRANSFORM,
} vk_cmd_kind;

typedef struct {
    vk_cmd_kind kind;
    union {
        struct { struct VkRenderPass_T *pass; struct VkFramebuffer_T *fb;
                 VkRect2D area; VkClearValue clear; int have_clear; } begin;
        struct { struct VkPipeline_T *pipeline; } bind_pipeline;
        struct { VkViewport vp; } viewport;
        struct { VkRect2D rect; } scissor;
        struct { struct VkBuffer_T *buffer; VkDeviceSize offset; } vertex;
        struct { struct VkBuffer_T *buffer; VkDeviceSize offset;
                 VkIndexType type; } index;
        struct { uint32_t count, first; } draw;
        struct { uint32_t count, first; int32_t vertex_offset; } draw_indexed;
        struct { float m[16]; } transform;
    } u;
} vk_cmd_t;

#define VK_MAX_COMMANDS 512

struct VkCommandBuffer_T {
    vk_cmd_t cmds[VK_MAX_COMMANDS];
    uint32_t count;
    int      recording;
};

/* ------------------------------------------------------------------ helpers */

static struct VkInstance_T       g_instance;
static struct VkPhysicalDevice_T g_phys;
static struct VkDevice_T         g_device;
static struct VkQueue_T          g_queue;
static struct VkCommandPool_T    g_pool;
static struct VkPipelineLayout_T g_layout;

static int format_components(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32_SFLOAT:       return 2;
    case VK_FORMAT_R32G32B32_SFLOAT:    return 3;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 4;
    default:                            return 0;
    }
}

/* ------------------------------------------------------------- instance etc */

VkResult vkCreateInstance(const VkInstanceCreateInfo *info, const void *alloc,
                          VkInstance *out) {
    (void)alloc;
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    if (info && info->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO)
        return VK_ERROR_INITIALIZATION_FAILED;
    g_instance.magic = 0x564B4954;
    *out = &g_instance;
    return VK_SUCCESS;
}

void vkDestroyInstance(VkInstance instance, const void *alloc) {
    (void)instance; (void)alloc;
}

VkResult vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
                                    VkPhysicalDevice *out) {
    (void)instance;
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!out) { *count = 1; return VK_SUCCESS; }
    if (*count == 0) return VK_INCOMPLETE;
    out[0] = &g_phys;
    *count = 1;
    return VK_SUCCESS;
}

void vkGetPhysicalDeviceProperties(VkPhysicalDevice dev,
                                   VkPhysicalDeviceProperties *out) {
    (void)dev;
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->apiVersion = (1u << 22) | (0u << 12) | 0u;      /* 1.0.0 */
    out->driverVersion = 1;
    out->vendorID = 0;
    out->deviceID = 0;
    /* A processor is what is actually executing this, and saying so is the
     * difference between a program choosing sensibly and one expecting a GPU. */
    out->deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    strlcpy(out->deviceName, "Kestrel software rasteriser", sizeof out->deviceName);
}

void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice dev, uint32_t *count,
                                              VkQueueFamilyProperties *out) {
    (void)dev;
    if (!count) return;
    if (!out) { *count = 1; return; }
    if (*count == 0) return;
    memset(out, 0, sizeof *out);
    out[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
    out[0].queueCount = 1;
    *count = 1;
}

void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice dev,
                                         VkPhysicalDeviceMemoryProperties *out) {
    (void)dev;
    if (!out) return;
    memset(out, 0, sizeof *out);
    /* One heap, ordinary memory: it is host visible and coherent because it is
     * the same memory the processor renders out of. */
    out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    out->memoryTypes[0].heapIndex = 0;
    out->memoryHeapCount = 1;
    out->memoryHeaps[0].size = 256u << 20;
    out->memoryHeaps[0].flags = 1;
}

VkResult vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *info,
                        const void *alloc, VkDevice *out) {
    (void)phys; (void)info; (void)alloc;
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    g_device.magic = 0x564B4445;
    g_queue.device = &g_device;
    g_queue.error = VK_SUCCESS;
    *out = &g_device;
    return VK_SUCCESS;
}

void vkDestroyDevice(VkDevice device, const void *alloc) { (void)device; (void)alloc; }

void vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out) {
    (void)device; (void)family; (void)index;
    if (out) *out = &g_queue;
}

/* ------------------------------------------------------------ memory, buffers */

VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *info,
                          const void *a, VkDeviceMemory *out) {
    (void)d; (void)a;
    if (!info || !out || !info->allocationSize) return VK_ERROR_OUT_OF_HOST_MEMORY;

    struct VkDeviceMemory_T *m = calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->host = malloc((size_t)info->allocationSize);
    if (!m->host) { free(m); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    m->size = info->allocationSize;
    *out = m;
    return VK_SUCCESS;
}

void vkFreeMemory(VkDevice d, VkDeviceMemory m, const void *a) {
    (void)d; (void)a;
    if (!m) return;
    free(m->host);
    free(m);
}

VkResult vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize offset,
                     VkDeviceSize size, VkFlags flags, void **out) {
    (void)d; (void)size; (void)flags;
    if (!m || !out || offset > m->size) return VK_ERROR_MEMORY_MAP_FAILED;
    m->mapped = 1;
    *out = (char *)m->host + offset;
    return VK_SUCCESS;
}

void vkUnmapMemory(VkDevice d, VkDeviceMemory m) {
    (void)d;
    if (m) m->mapped = 0;
}

VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *info,
                        const void *a, VkBuffer *out) {
    (void)d; (void)a;
    if (!info || !out) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct VkBuffer_T *b = calloc(1, sizeof *b);
    if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    b->size = info->size;
    b->usage = info->usage;
    *out = b;
    return VK_SUCCESS;
}

void vkDestroyBuffer(VkDevice d, VkBuffer b, const void *a) {
    (void)d; (void)a;
    free(b);
}

void vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *out) {
    (void)d;
    if (!out) return;
    out->size = b ? b->size : 0;
    out->alignment = 16;
    out->memoryTypeBits = 1;
}

VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m,
                            VkDeviceSize offset) {
    (void)d;
    if (!b || !m || offset + b->size > m->size) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    b->memory = m;
    b->offset = offset;
    return VK_SUCCESS;
}

/* -------------------------------------------------------------- images, views */

VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo *info,
                       const void *a, VkImage *out) {
    (void)d; (void)a;
    if (!info || !out) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct VkImage_T *i = calloc(1, sizeof *i);
    if (!i) return VK_ERROR_OUT_OF_HOST_MEMORY;
    i->width = info->extent.width;
    i->height = info->extent.height;
    i->format = info->format;
    *out = i;
    return VK_SUCCESS;
}

void vkDestroyImage(VkDevice d, VkImage i, const void *a) {
    (void)d; (void)a;
    free(i);
}

VkResult vkBindImageToSurface(VkImage image, void *colour_surface) {
    if (!image || !colour_surface) return VK_ERROR_INITIALIZATION_FAILED;
    surface_t *s = colour_surface;
    image->surface = s;
    image->width = (uint32_t)s->width;
    image->height = (uint32_t)s->height;
    return VK_SUCCESS;
}

VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *info,
                           const void *a, VkImageView *out) {
    (void)d; (void)a;
    if (!info || !out || !info->image) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkImageView_T *v = calloc(1, sizeof *v);
    if (!v) return VK_ERROR_OUT_OF_HOST_MEMORY;
    v->image = info->image;
    *out = v;
    return VK_SUCCESS;
}

void vkDestroyImageView(VkDevice d, VkImageView v, const void *a) {
    (void)d; (void)a;
    free(v);
}

/* ------------------------------------------------------- passes, framebuffers */

VkResult vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *info,
                            const void *a, VkRenderPass *out) {
    (void)d; (void)a;
    if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkRenderPass_T *r = calloc(1, sizeof *r);
    if (!r) return VK_ERROR_OUT_OF_HOST_MEMORY;

    r->load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    if (info->attachmentCount && info->pAttachments)
        r->load_op = info->pAttachments[0].loadOp;
    if (info->subpassCount && info->pSubpasses)
        r->has_depth = info->pSubpasses[0].pDepthStencilAttachment != NULL;

    *out = r;
    return VK_SUCCESS;
}

void vkDestroyRenderPass(VkDevice d, VkRenderPass r, const void *a) {
    (void)d; (void)a;
    free(r);
}

VkResult vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *info,
                             const void *a, VkFramebuffer *out) {
    (void)d; (void)a;
    if (!info || !out || !info->attachmentCount || !info->pAttachments)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct VkFramebuffer_T *f = calloc(1, sizeof *f);
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    f->colour = info->pAttachments[0];
    f->width = info->width;
    f->height = info->height;
    *out = f;
    return VK_SUCCESS;
}

void vkDestroyFramebuffer(VkDevice d, VkFramebuffer f, const void *a) {
    (void)d; (void)a;
    free(f);
}

/* ----------------------------------------------------------------- pipelines */

VkResult vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *info,
                              const void *a, VkShaderModule *out) {
    (void)d; (void)a;
    if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkShaderModule_T *m = calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    /* The code is kept only so the object has a size to report; nothing here
     * can execute SPIR-V. */
    m->code_size = info->codeSize;
    *out = m;
    return VK_SUCCESS;
}

void vkDestroyShaderModule(VkDevice d, VkShaderModule m, const void *a) {
    (void)d; (void)a;
    free(m);
}

VkResult vkCreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *info,
                                const void *a, VkPipelineLayout *out) {
    (void)d; (void)info; (void)a;
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = &g_layout;
    return VK_SUCCESS;
}

void vkDestroyPipelineLayout(VkDevice d, VkPipelineLayout l, const void *a) {
    (void)d; (void)l; (void)a;
}

VkResult vkCreateGraphicsPipelines(VkDevice d, void *cache, uint32_t count,
                                   const VkGraphicsPipelineCreateInfo *infos,
                                   const void *a, VkPipeline *out) {
    (void)d; (void)cache; (void)a;
    if (!infos || !out) return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < count; i++) {
        struct VkPipeline_T *p = calloc(1, sizeof *p);
        if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;

        p->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        p->front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        p->depth_compare = VK_COMPARE_OP_LESS;
        p->blend_src = VK_BLEND_FACTOR_ONE;
        p->blend_dst = VK_BLEND_FACTOR_ZERO;

        const VkGraphicsPipelineCreateInfo *ci = &infos[i];

        if (ci->pInputAssemblyState) p->topology = ci->pInputAssemblyState->topology;

        if (ci->pRasterizationState) {
            p->cull_mode = ci->pRasterizationState->cullMode;
            p->front_face = ci->pRasterizationState->frontFace;
        }

        if (ci->pDepthStencilState) {
            p->depth_test = ci->pDepthStencilState->depthTestEnable != 0;
            p->depth_write = ci->pDepthStencilState->depthWriteEnable != 0;
            p->depth_compare = ci->pDepthStencilState->depthCompareOp;
        }

        if (ci->pColorBlendState && ci->pColorBlendState->attachmentCount &&
            ci->pColorBlendState->pAttachments) {
            const VkPipelineColorBlendAttachmentState *b =
                &ci->pColorBlendState->pAttachments[0];
            p->blend = b->blendEnable != 0;
            p->blend_src = b->srcColorBlendFactor;
            p->blend_dst = b->dstColorBlendFactor;
        }

        if (ci->pViewportState && ci->pViewportState->viewportCount &&
            ci->pViewportState->pViewports) {
            p->viewport = ci->pViewportState->pViewports[0];
            p->have_viewport = 1;
        }

        /* The vertex input description is what this pipeline is really made
         * of: without a shader to run, the attribute locations are the only
         * statement of what each part of a vertex means.  Location 0 is the
         * position, 1 the colour and 2 the texture coordinate. */
        const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
        if (vi) {
            if (vi->vertexBindingDescriptionCount && vi->pVertexBindingDescriptions)
                p->binding_stride = vi->pVertexBindingDescriptions[0].stride;

            for (uint32_t k = 0; k < vi->vertexAttributeDescriptionCount &&
                                 p->attr_count < VK_MAX_ATTRS; k++) {
                const VkVertexInputAttributeDescription *ad =
                    &vi->pVertexAttributeDescriptions[k];
                int comps = format_components(ad->format);
                if (!comps) continue;
                p->attr[p->attr_count].location = ad->location;
                p->attr[p->attr_count].offset = ad->offset;
                p->attr[p->attr_count].format = ad->format;
                p->attr[p->attr_count].components = comps;
                p->attr_count++;
            }
        }

        out[i] = p;
    }
    return VK_SUCCESS;
}

void vkDestroyPipeline(VkDevice d, VkPipeline p, const void *a) {
    (void)d; (void)a;
    free(p);
}

/* ------------------------------------------------------------ command buffers */

VkResult vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *info,
                             const void *a, VkCommandPool *out) {
    (void)d; (void)info; (void)a;
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = &g_pool;
    return VK_SUCCESS;
}

void vkDestroyCommandPool(VkDevice d, VkCommandPool p, const void *a) {
    (void)d; (void)p; (void)a;
}

VkResult vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *info,
                                  VkCommandBuffer *out) {
    (void)d;
    if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < info->commandBufferCount; i++) {
        out[i] = calloc(1, sizeof(struct VkCommandBuffer_T));
        if (!out[i]) return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

void vkFreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t count,
                          const VkCommandBuffer *bufs) {
    (void)d; (void)p;
    if (!bufs) return;
    for (uint32_t i = 0; i < count; i++) free(bufs[i]);
}

VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    (void)info;
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    cb->count = 0;
    cb->recording = 1;
    return VK_SUCCESS;
}

VkResult vkEndCommandBuffer(VkCommandBuffer cb) {
    if (!cb || !cb->recording) return VK_ERROR_INITIALIZATION_FAILED;
    cb->recording = 0;
    return VK_SUCCESS;
}

VkResult vkResetCommandBuffer(VkCommandBuffer cb, VkFlags flags) {
    (void)flags;
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    cb->count = 0;
    cb->recording = 0;
    return VK_SUCCESS;
}

static vk_cmd_t *record(VkCommandBuffer cb, vk_cmd_kind kind) {
    if (!cb || !cb->recording || cb->count >= VK_MAX_COMMANDS) return NULL;
    vk_cmd_t *c = &cb->cmds[cb->count++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    return c;
}

void vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info,
                          VkSubpassContents contents) {
    (void)contents;
    vk_cmd_t *c = record(cb, CMD_BEGIN_PASS);
    if (!c || !info) return;
    c->u.begin.pass = info->renderPass;
    c->u.begin.fb = info->framebuffer;
    c->u.begin.area = info->renderArea;
    if (info->clearValueCount && info->pClearValues) {
        c->u.begin.clear = info->pClearValues[0];
        c->u.begin.have_clear = 1;
    }
}

void vkCmdEndRenderPass(VkCommandBuffer cb) { record(cb, CMD_END_PASS); }

void vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipeline p) {
    if (bind != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    vk_cmd_t *c = record(cb, CMD_BIND_PIPELINE);
    if (c) c->u.bind_pipeline.pipeline = p;
}

void vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                      const VkViewport *viewports) {
    (void)first;
    if (!count || !viewports) return;
    vk_cmd_t *c = record(cb, CMD_SET_VIEWPORT);
    if (c) c->u.viewport.vp = viewports[0];
}

void vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count,
                     const VkRect2D *scissors) {
    (void)first;
    if (!count || !scissors) return;
    vk_cmd_t *c = record(cb, CMD_SET_SCISSOR);
    if (c) c->u.scissor.rect = scissors[0];
}

void vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                            const VkBuffer *buffers, const VkDeviceSize *offsets) {
    (void)first;
    if (!count || !buffers) return;
    vk_cmd_t *c = record(cb, CMD_BIND_VERTEX);
    if (!c) return;
    c->u.vertex.buffer = buffers[0];
    c->u.vertex.offset = offsets ? offsets[0] : 0;
}

void vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset,
                          VkIndexType type) {
    vk_cmd_t *c = record(cb, CMD_BIND_INDEX);
    if (!c) return;
    c->u.index.buffer = b;
    c->u.index.offset = offset;
    c->u.index.type = type;
}

void vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount, uint32_t instanceCount,
               uint32_t firstVertex, uint32_t firstInstance) {
    (void)instanceCount; (void)firstInstance;
    vk_cmd_t *c = record(cb, CMD_DRAW);
    if (!c) return;
    c->u.draw.count = vertexCount;
    c->u.draw.first = firstVertex;
}

void vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount,
                      uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    (void)instanceCount; (void)firstInstance;
    vk_cmd_t *c = record(cb, CMD_DRAW_INDEXED);
    if (!c) return;
    c->u.draw_indexed.count = indexCount;
    c->u.draw_indexed.first = firstIndex;
    c->u.draw_indexed.vertex_offset = vertexOffset;
}

void vkCmdSetTransformKESTREL(VkCommandBuffer cb, const float *matrix) {
    vk_cmd_t *c = record(cb, CMD_SET_TRANSFORM);
    if (!c || !matrix) return;
    memcpy(c->u.transform.m, matrix, sizeof c->u.transform.m);
}

/* ------------------------------------------------------------------ execution */

/* The state a replay carries between commands. */
typedef struct {
    struct VkPipeline_T *pipeline;
    const unsigned char *vertex_base;
    const unsigned char *index_base;
    VkIndexType index_type;
    surface_t *target;
    bool have_viewport;
} vk_exec_t;

static void read_attr(const vk_exec_t *e, uint32_t index, uint32_t location,
                      float *out, int want) {
    for (int i = 0; i < want; i++) out[i] = 0.0f;
    struct VkPipeline_T *p = e->pipeline;
    if (!p || !e->vertex_base) return;

    for (uint32_t k = 0; k < p->attr_count; k++) {
        if (p->attr[k].location != location) continue;
        const float *src = (const float *)(e->vertex_base +
                                           (size_t)index * p->binding_stride +
                                           p->attr[k].offset);
        int n = p->attr[k].components < want ? p->attr[k].components : want;
        for (int i = 0; i < n; i++) out[i] = src[i];
        return;
    }
}

static GLenum topology_to_gl(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:     return GL_POINTS;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:      return GL_LINES;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return GL_LINE_STRIP;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
    default:                                   return GL_TRIANGLES;
    }
}

static GLenum compare_to_gl(VkCompareOp op) {
    switch (op) {
    case VK_COMPARE_OP_NEVER:            return GL_NEVER;
    case VK_COMPARE_OP_EQUAL:            return GL_EQUAL;
    case VK_COMPARE_OP_LESS_OR_EQUAL:    return GL_LEQUAL;
    case VK_COMPARE_OP_GREATER:          return GL_GREATER;
    case VK_COMPARE_OP_NOT_EQUAL:        return GL_NOTEQUAL;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return GL_GEQUAL;
    case VK_COMPARE_OP_ALWAYS:           return GL_ALWAYS;
    default:                             return GL_LESS;
    }
}

static GLenum blend_to_gl(VkBlendFactor f) {
    switch (f) {
    case VK_BLEND_FACTOR_ZERO:                return GL_ZERO;
    case VK_BLEND_FACTOR_SRC_ALPHA:           return GL_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    default:                                  return GL_ONE;
    }
}

static void apply_pipeline(vk_exec_t *e) {
    struct VkPipeline_T *p = e->pipeline;
    if (!p) return;

    if (p->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(p->depth_write ? GL_TRUE : GL_FALSE);
    glDepthFunc(compare_to_gl(p->depth_compare));

    if (p->cull_mode == VK_CULL_MODE_NONE) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(p->cull_mode == VK_CULL_MODE_FRONT_BIT ? GL_FRONT :
                   p->cull_mode == VK_CULL_MODE_FRONT_AND_BACK ? GL_FRONT_AND_BACK :
                   GL_BACK);
    }
    /* Vulkan names its front face by the winding in framebuffer coordinates,
     * which is the space the rasteriser measures in once the y-down convention
     * is set, so the winding passes straight through. */
    glFrontFace(p->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE ? GL_CCW : GL_CW);

    if (p->blend) {
        glEnable(GL_BLEND);
        glBlendFunc(blend_to_gl(p->blend_src), blend_to_gl(p->blend_dst));
    } else {
        glDisable(GL_BLEND);
    }

    if (p->have_viewport) {
        glViewport((GLint)p->viewport.x, (GLint)p->viewport.y,
                   (GLsizei)p->viewport.width, (GLsizei)p->viewport.height);
        glDepthRange(p->viewport.minDepth, p->viewport.maxDepth);
        e->have_viewport = true;
    }
}

static void emit_vertex(const vk_exec_t *e, uint32_t index) {
    float pos[4] = { 0, 0, 0, 1 };
    float col[4] = { 1, 1, 1, 1 };
    float uv[2]  = { 0, 0 };

    read_attr(e, index, 0, pos, 4);
    read_attr(e, index, 1, col, 4);
    read_attr(e, index, 2, uv, 2);

    /* A three-component position leaves w at one, which is what a vertex
     * shader would have produced for an ordinary model. */
    struct VkPipeline_T *p = e->pipeline;
    for (uint32_t k = 0; k < (p ? p->attr_count : 0); k++)
        if (p->attr[k].location == 0 && p->attr[k].components < 4) pos[3] = 1.0f;

    glColor4f(col[0], col[1], col[2], col[3]);
    glTexCoord2f(uv[0], uv[1]);
    glVertex4f(pos[0], pos[1], pos[2], pos[3]);
}

static VkResult execution_error(void) {
    /* Leave the shared GL error visible to callers of the bridge. */
    if (g_gl.error == GL_OUT_OF_MEMORY) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (g_gl.error != GL_NO_ERROR) return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}

static VkResult execute(VkCommandBuffer cb) {
    vk_exec_t e;
    memset(&e, 0, sizeof e);

    /* Retire earlier GL work before replacing its state. Unsupported Vulkan
     * shader/material features must not inherit arbitrary GL side effects. */
    gl_context_init();
    if (!gl_gpu_flush() || !gui_gpu_flush()) return VK_ERROR_DEVICE_LOST;
    glUseProgram(0);
    glDisable(GL_LIGHTING);glDisable(GL_COLOR_MATERIAL);glDisable(GL_NORMALIZE);
    glDisable(GL_TEXTURE_2D);glDisable(GL_ALPHA_TEST);glDisable(GL_SCISSOR_TEST);
    glShadeModel(GL_SMOOTH);glPolygonOffset(0,0);glAlphaFunc(GL_ALWAYS,0);
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);glBlendFunc(GL_ONE,GL_ZERO);
    glDisable(GL_CULL_FACE);glCullFace(GL_BACK);glFrontFace(GL_CCW);
    glDepthRange(0.0,1.0);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    gl_set_clip_depth_zero_to_one(true);gl_set_clip_y_down(true);
    if (execution_error()!=VK_SUCCESS) return execution_error();

    for (uint32_t i = 0; i < cb->count; i++) {
        const vk_cmd_t *c = &cb->cmds[i];

        switch (c->kind) {
        case CMD_BEGIN_PASS: {
            struct VkFramebuffer_T *fb = c->u.begin.fb;
            if (!fb || !fb->colour || !fb->colour->image)
                return VK_ERROR_INITIALIZATION_FAILED;
            e.target = fb->colour->image->surface;
            if (!e.target) return VK_ERROR_INITIALIZATION_FAILED;

            glSetTarget(e.target);
            if (execution_error()!=VK_SUCCESS) return execution_error();
            if (!e.have_viewport) {
                glViewport(c->u.begin.area.offset.x,c->u.begin.area.offset.y,
                           (GLsizei)c->u.begin.area.extent.width,
                           (GLsizei)c->u.begin.area.extent.height);
                glDepthRange(0.0,1.0);
            }

            /* Vulkan's near plane is at zero where OpenGL's is at minus one. */
            gl_set_clip_depth_zero_to_one(true);
            gl_set_clip_y_down(true);

            if (c->u.begin.pass &&
                c->u.begin.pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                c->u.begin.have_clear) {
                /* Clear uses renderArea, independently of draw viewport and
                 * scissor. Preserve explicit commands recorded before Begin. */
                int vx=g_gl.vp_x,vy=g_gl.vp_y,vw=g_gl.vp_w,vh=g_gl.vp_h;
                float near_depth=g_gl.depth_near,far_depth=g_gl.depth_far;
                bool scissor=g_gl.scissor_on;
                glDisable(GL_SCISSOR_TEST);
                glViewport(c->u.begin.area.offset.x,c->u.begin.area.offset.y,
                    (GLsizei)c->u.begin.area.extent.width,(GLsizei)c->u.begin.area.extent.height);
                glDepthRange(0.0,1.0);
                if (execution_error()!=VK_SUCCESS) {
                    glViewport(vx,vy,vw,vh);glDepthRange(near_depth,far_depth);
                    if(scissor)glEnable(GL_SCISSOR_TEST);
                    return execution_error();
                }
                const float *rgba = c->u.begin.clear.color.float32;
                glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
                glClearDepth(1.0);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glViewport(vx,vy,vw,vh);glDepthRange(near_depth,far_depth);
                if(scissor)glEnable(GL_SCISSOR_TEST);
            }

            // Identity is established once at entry; preserve pre-pass transforms.
            break;
        }

        case CMD_END_PASS:
            break;

        case CMD_BIND_PIPELINE:
            e.pipeline = c->u.bind_pipeline.pipeline;
            apply_pipeline(&e);
            break;

        case CMD_SET_VIEWPORT:
            glViewport((GLint)c->u.viewport.vp.x, (GLint)c->u.viewport.vp.y,
                       (GLsizei)c->u.viewport.vp.width,
                       (GLsizei)c->u.viewport.vp.height);
            glDepthRange(c->u.viewport.vp.minDepth,c->u.viewport.vp.maxDepth);
            e.have_viewport = true;
            break;

        case CMD_SET_SCISSOR:
            glEnable(GL_SCISSOR_TEST);
            glScissor(c->u.scissor.rect.offset.x, c->u.scissor.rect.offset.y,
                      (GLsizei)c->u.scissor.rect.extent.width,
                      (GLsizei)c->u.scissor.rect.extent.height);
            break;

        case CMD_BIND_VERTEX:
            e.vertex_base = NULL;
            if (c->u.vertex.buffer && c->u.vertex.buffer->memory)
                e.vertex_base = (const unsigned char *)c->u.vertex.buffer->memory->host +
                                c->u.vertex.buffer->offset + c->u.vertex.offset;
            break;

        case CMD_BIND_INDEX:
            e.index_base = NULL;
            e.index_type = c->u.index.type;
            if (c->u.index.buffer && c->u.index.buffer->memory)
                e.index_base = (const unsigned char *)c->u.index.buffer->memory->host +
                               c->u.index.buffer->offset + c->u.index.offset;
            break;

        case CMD_SET_TRANSFORM:
            glMatrixMode(GL_MODELVIEW);
            gl_set_clip_depth_zero_to_one(true);   /* glMatrixMode clears both */
            gl_set_clip_y_down(true);
            glLoadMatrixf(c->u.transform.m);
            break;

        case CMD_DRAW:
            if (!e.target || !e.pipeline || !e.vertex_base)
                return VK_ERROR_INITIALIZATION_FAILED;
            glBegin(topology_to_gl(e.pipeline->topology));
            for (uint32_t v = 0; v < c->u.draw.count; v++)
                emit_vertex(&e, c->u.draw.first + v);
            glEnd();
            break;

        case CMD_DRAW_INDEXED:
            if (!e.target || !e.pipeline || !e.vertex_base || !e.index_base)
                return VK_ERROR_INITIALIZATION_FAILED;
            glBegin(topology_to_gl(e.pipeline->topology));
            for (uint32_t k = 0; k < c->u.draw_indexed.count; k++) {
                uint32_t idx = k + c->u.draw_indexed.first;
                uint32_t v = (e.index_type == VK_INDEX_TYPE_UINT16)
                           ? ((const uint16_t *)e.index_base)[idx]
                           : ((const uint32_t *)e.index_base)[idx];
                emit_vertex(&e, (uint32_t)((int32_t)v + c->u.draw_indexed.vertex_offset));
            }
            glEnd();
            break;
        default:
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkResult error = execution_error();
        if (error != VK_SUCCESS) return error;
    }

    glDisable(GL_SCISSOR_TEST);
    /* Submit is synchronous in this subset. Do not signal completion while
     * clear/UI work is still queued, or after a backend flush failed. */
    if (!gl_gpu_flush() || !gui_gpu_flush()) return VK_ERROR_DEVICE_LOST;
    return execution_error();
}

VkResult vkQueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo *submits,
                       VkFence fence) {
    if (q != &g_queue || q->device != &g_device ||
        (count && !submits) || (fence && fence->signalled))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (q->error != VK_SUCCESS) return q->error;
    if (g_gl.in_begin || execution_error() != VK_SUCCESS)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* Validate the entire list before executing any buffer. An already
     * signalled input fence is rejected above without changing its state. */
    for (uint32_t i = 0; i < count; i++) {
        const VkSubmitInfo *s = &submits[i];
        if (s->waitSemaphoreCount || s->signalSemaphoreCount)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (s->commandBufferCount && !s->pCommandBuffers)
            return VK_ERROR_INITIALIZATION_FAILED;
        for (uint32_t k = 0; k < s->commandBufferCount; k++)
            if (!s->pCommandBuffers[k] || s->pCommandBuffers[k]->recording)
                return VK_ERROR_INITIALIZATION_FAILED;
    }

    for (uint32_t i = 0; i < count; i++) {
        const VkSubmitInfo *s = &submits[i];
        for (uint32_t k = 0; k < s->commandBufferCount; k++) {
            VkResult error = execute(s->pCommandBuffers[k]);
            if (error != VK_SUCCESS) {
                /* Earlier commands may have executed. Do not replay, signal
                 * the fence, or let WaitIdle hide the failed submission. */
                q->error = error;
                return error;
            }
        }
    }
    if (fence) fence->signalled = 1;
    return VK_SUCCESS;
}

/* Work is finished by the time submit returns, so waiting has nothing to do. */
VkResult vkQueueWaitIdle(VkQueue q) {
    return q == &g_queue && q->device == &g_device
         ? q->error : VK_ERROR_INITIALIZATION_FAILED;
}
VkResult vkDeviceWaitIdle(VkDevice d) {
    return d == &g_device && g_queue.device == d
         ? g_queue.error : VK_ERROR_INITIALIZATION_FAILED;
}
