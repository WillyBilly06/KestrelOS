/* vulkan.h - a core subset of Vulkan 1.0 for KestrelOS.
 *
 * What is here is the path a program actually takes to get a triangle on the
 * screen: an instance, a physical device, a logical device and queue, buffers
 * backed by memory, a render pass, a graphics pipeline, a command buffer, and a
 * submit.  A program written against that path compiles and runs.
 *
 * What is not here, and cannot honestly be: SPIR-V.  Vulkan's whole design is
 * that the pipeline is defined by compiled shaders, and there is no shader
 * compiler on this system and no programmable hardware under it.  So a pipeline
 * created here is driven by its VkPipelineVertexInputStateCreateInfo and its
 * fixed-function state, and the shader modules are accepted and recorded but
 * not executed.  Everything else - the object model, the lifetimes, the
 * validation of what a call is allowed to do - is real.
 *
 * That is a subset, and it is written down here rather than discovered at
 * run time.
 */
#ifndef KESTREL_VULKAN_H
#define KESTREL_VULKAN_H

#include <stdint.h>
#include <stddef.h>

#define VKAPI_CALL
#define VKAPI_PTR

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkSampleMask;

#define VK_NULL_HANDLE ((void *)0)
#define VK_TRUE  1
#define VK_FALSE 0

#define VK_DEFINE_HANDLE(name) typedef struct name##_T *name;
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_HANDLE(VkBuffer)
VK_DEFINE_HANDLE(VkImage)
VK_DEFINE_HANDLE(VkImageView)
VK_DEFINE_HANDLE(VkDeviceMemory)
VK_DEFINE_HANDLE(VkRenderPass)
VK_DEFINE_HANDLE(VkFramebuffer)
VK_DEFINE_HANDLE(VkPipeline)
VK_DEFINE_HANDLE(VkPipelineLayout)
VK_DEFINE_HANDLE(VkShaderModule)
VK_DEFINE_HANDLE(VkCommandPool)
VK_DEFINE_HANDLE(VkFence)

typedef enum {
    VK_SUCCESS = 0,
    VK_NOT_READY = 1,
    VK_TIMEOUT = 2,
    VK_INCOMPLETE = 5,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_DEVICE_LOST = -4,
    VK_ERROR_MEMORY_MAP_FAILED = -5,
    VK_ERROR_FEATURE_NOT_PRESENT = -8,
    VK_ERROR_FORMAT_NOT_SUPPORTED = -11,
} VkResult;

typedef enum {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19,
    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22,
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24,
    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO = 25,
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26,
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
    VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37,
    VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
} VkStructureType;

typedef enum {
    VK_FORMAT_UNDEFINED = 0,
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_B8G8R8A8_UNORM = 44,
    VK_FORMAT_R32G32_SFLOAT = 103,
    VK_FORMAT_R32G32B32_SFLOAT = 106,
    VK_FORMAT_R32G32B32A32_SFLOAT = 109,
    VK_FORMAT_D32_SFLOAT = 126,
} VkFormat;

typedef enum {
    VK_PRIMITIVE_TOPOLOGY_POINT_LIST = 0,
    VK_PRIMITIVE_TOPOLOGY_LINE_LIST = 1,
    VK_PRIMITIVE_TOPOLOGY_LINE_STRIP = 2,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 4,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN = 5,
} VkPrimitiveTopology;

typedef enum {
    VK_CULL_MODE_NONE = 0,
    VK_CULL_MODE_FRONT_BIT = 1,
    VK_CULL_MODE_BACK_BIT = 2,
    VK_CULL_MODE_FRONT_AND_BACK = 3,
} VkCullModeFlagBits;

typedef enum {
    VK_FRONT_FACE_COUNTER_CLOCKWISE = 0,
    VK_FRONT_FACE_CLOCKWISE = 1,
} VkFrontFace;

typedef enum {
    VK_COMPARE_OP_NEVER = 0, VK_COMPARE_OP_LESS = 1, VK_COMPARE_OP_EQUAL = 2,
    VK_COMPARE_OP_LESS_OR_EQUAL = 3, VK_COMPARE_OP_GREATER = 4,
    VK_COMPARE_OP_NOT_EQUAL = 5, VK_COMPARE_OP_GREATER_OR_EQUAL = 6,
    VK_COMPARE_OP_ALWAYS = 7,
} VkCompareOp;

typedef enum {
    VK_SHADER_STAGE_VERTEX_BIT = 0x01,
    VK_SHADER_STAGE_FRAGMENT_BIT = 0x10,
} VkShaderStageFlagBits;

typedef enum {
    VK_VERTEX_INPUT_RATE_VERTEX = 0,
    VK_VERTEX_INPUT_RATE_INSTANCE = 1,
} VkVertexInputRate;

typedef enum {
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x00000080,
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT = 0x00000040,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x00000001,
} VkBufferUsageFlagBits;

typedef enum {
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x01,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x02,
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x04,
} VkMemoryPropertyFlagBits;

typedef enum {
    VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
    VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
    VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
    VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
} VkPhysicalDeviceType;

typedef enum {
    VK_QUEUE_GRAPHICS_BIT = 0x01,
    VK_QUEUE_TRANSFER_BIT = 0x04,
} VkQueueFlagBits;

typedef enum {
    VK_SUBPASS_CONTENTS_INLINE = 0,
} VkSubpassContents;

typedef enum {
    VK_PIPELINE_BIND_POINT_GRAPHICS = 0,
} VkPipelineBindPoint;

typedef enum {
    VK_ATTACHMENT_LOAD_OP_LOAD = 0,
    VK_ATTACHMENT_LOAD_OP_CLEAR = 1,
    VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2,
} VkAttachmentLoadOp;

typedef enum {
    VK_ATTACHMENT_STORE_OP_STORE = 0,
    VK_ATTACHMENT_STORE_OP_DONT_CARE = 1,
} VkAttachmentStoreOp;

typedef enum { VK_IMAGE_LAYOUT_UNDEFINED = 0, VK_IMAGE_LAYOUT_GENERAL = 1,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002 } VkImageLayout;
typedef enum { VK_IMAGE_TYPE_2D = 1 } VkImageType;
typedef enum { VK_IMAGE_VIEW_TYPE_2D = 1 } VkImageViewType;
typedef enum { VK_SAMPLE_COUNT_1_BIT = 1 } VkSampleCountFlagBits;
typedef enum { VK_POLYGON_MODE_FILL = 0, VK_POLYGON_MODE_LINE = 1,
               VK_POLYGON_MODE_POINT = 2 } VkPolygonMode;
typedef enum { VK_SHARING_MODE_EXCLUSIVE = 0 } VkSharingMode;
typedef enum { VK_INDEX_TYPE_UINT16 = 0, VK_INDEX_TYPE_UINT32 = 1 } VkIndexType;
typedef enum { VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0 } VkCommandBufferLevel;
typedef enum { VK_BLEND_FACTOR_ZERO = 0, VK_BLEND_FACTOR_ONE = 1,
               VK_BLEND_FACTOR_SRC_ALPHA = 6,
               VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7 } VkBlendFactor;
typedef enum { VK_BLEND_OP_ADD = 0 } VkBlendOp;

typedef struct { int32_t x, y; } VkOffset2D;
typedef struct { uint32_t width, height; } VkExtent2D;
typedef struct { uint32_t width, height, depth; } VkExtent3D;
typedef struct { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct { float x, y, width, height, minDepth, maxDepth; } VkViewport;

typedef union {
    float    float32[4];
    int32_t  int32[4];
    uint32_t uint32[4];
} VkClearColorValue;

typedef struct { float depth; uint32_t stencil; } VkClearDepthStencilValue;

typedef union {
    VkClearColorValue        color;
    VkClearDepthStencilValue depthStencil;
} VkClearValue;

typedef struct {
    VkStructureType sType; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion;
    const char *pEngineName; uint32_t engineVersion; uint32_t apiVersion;
} VkApplicationInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct {
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    VkPhysicalDeviceType deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
} VkPhysicalDeviceProperties;

typedef struct {
    VkFlags queueFlags; uint32_t queueCount;
    uint32_t timestampValidBits; VkExtent3D minImageTransferGranularity;
} VkQueueFamilyProperties;

typedef struct { VkFlags propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct { VkDeviceSize size; VkFlags flags; } VkMemoryHeap;
typedef struct {
    uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    uint32_t queueFamilyIndex, queueCount; const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkDeviceSize size; VkFlags usage; VkSharingMode sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct {
    VkDeviceSize size, alignment; uint32_t memoryTypeBits;
} VkMemoryRequirements;

typedef struct {
    VkStructureType sType; const void *pNext;
    VkDeviceSize allocationSize; uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkImageType imageType; VkFormat format; VkExtent3D extent;
    uint32_t mipLevels, arrayLayers; VkSampleCountFlagBits samples;
    uint32_t tiling; VkFlags usage; VkSharingMode sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices;
    VkImageLayout initialLayout;
} VkImageCreateInfo;

typedef struct {
    uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount;
} VkImageSubresourceRange;

typedef struct { uint32_t r, g, b, a; } VkComponentMapping;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkImage image; VkImageViewType viewType; VkFormat format;
    VkComponentMapping components; VkImageSubresourceRange subresourceRange;
} VkImageViewCreateInfo;

typedef struct {
    VkFlags flags; VkFormat format; VkSampleCountFlagBits samples;
    VkAttachmentLoadOp loadOp; VkAttachmentStoreOp storeOp;
    VkAttachmentLoadOp stencilLoadOp; VkAttachmentStoreOp stencilStoreOp;
    VkImageLayout initialLayout, finalLayout;
} VkAttachmentDescription;

typedef struct { uint32_t attachment; VkImageLayout layout; } VkAttachmentReference;

typedef struct {
    VkFlags flags; VkPipelineBindPoint pipelineBindPoint;
    uint32_t inputAttachmentCount; const VkAttachmentReference *pInputAttachments;
    uint32_t colorAttachmentCount; const VkAttachmentReference *pColorAttachments;
    const VkAttachmentReference *pResolveAttachments;
    const VkAttachmentReference *pDepthStencilAttachment;
    uint32_t preserveAttachmentCount; const uint32_t *pPreserveAttachments;
} VkSubpassDescription;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    uint32_t attachmentCount; const VkAttachmentDescription *pAttachments;
    uint32_t subpassCount; const VkSubpassDescription *pSubpasses;
    uint32_t dependencyCount; const void *pDependencies;
} VkRenderPassCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkRenderPass renderPass; uint32_t attachmentCount;
    const VkImageView *pAttachments; uint32_t width, height, layers;
} VkFramebufferCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    size_t codeSize; const uint32_t *pCode;
} VkShaderModuleCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkShaderStageFlagBits stage; VkShaderModule module;
    const char *pName; const void *pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct {
    uint32_t binding, stride; VkVertexInputRate inputRate;
} VkVertexInputBindingDescription;

typedef struct {
    uint32_t location, binding; VkFormat format; uint32_t offset;
} VkVertexInputAttributeDescription;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    uint32_t vertexBindingDescriptionCount;
    const VkVertexInputBindingDescription *pVertexBindingDescriptions;
    uint32_t vertexAttributeDescriptionCount;
    const VkVertexInputAttributeDescription *pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkPrimitiveTopology topology; VkBool32 primitiveRestartEnable;
} VkPipelineInputAssemblyStateCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    uint32_t viewportCount; const VkViewport *pViewports;
    uint32_t scissorCount; const VkRect2D *pScissors;
} VkPipelineViewportStateCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkBool32 depthClampEnable, rasterizerDiscardEnable;
    VkPolygonMode polygonMode; VkFlags cullMode; VkFrontFace frontFace;
    VkBool32 depthBiasEnable;
    float depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor;
    float lineWidth;
} VkPipelineRasterizationStateCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkSampleCountFlagBits rasterizationSamples; VkBool32 sampleShadingEnable;
    float minSampleShading; const VkSampleMask *pSampleMask;
    VkBool32 alphaToCoverageEnable, alphaToOneEnable;
} VkPipelineMultisampleStateCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkBool32 depthTestEnable, depthWriteEnable; VkCompareOp depthCompareOp;
    VkBool32 depthBoundsTestEnable, stencilTestEnable;
    uint8_t front[28], back[28];
    float minDepthBounds, maxDepthBounds;
} VkPipelineDepthStencilStateCreateInfo;

typedef struct {
    VkBool32 blendEnable;
    VkBlendFactor srcColorBlendFactor, dstColorBlendFactor; VkBlendOp colorBlendOp;
    VkBlendFactor srcAlphaBlendFactor, dstAlphaBlendFactor; VkBlendOp alphaBlendOp;
    VkFlags colorWriteMask;
} VkPipelineColorBlendAttachmentState;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    VkBool32 logicOpEnable; uint32_t logicOp;
    uint32_t attachmentCount;
    const VkPipelineColorBlendAttachmentState *pAttachments;
    float blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    uint32_t setLayoutCount; const void *pSetLayouts;
    uint32_t pushConstantRangeCount; const void *pPushConstantRanges;
} VkPipelineLayoutCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    uint32_t stageCount; const VkPipelineShaderStageCreateInfo *pStages;
    const VkPipelineVertexInputStateCreateInfo *pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *pInputAssemblyState;
    const void *pTessellationState;
    const VkPipelineViewportStateCreateInfo *pViewportState;
    const VkPipelineRasterizationStateCreateInfo *pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo *pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfo *pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfo *pColorBlendState;
    const void *pDynamicState;
    VkPipelineLayout layout; VkRenderPass renderPass; uint32_t subpass;
    VkPipeline basePipelineHandle; int32_t basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct {
    VkStructureType sType; const void *pNext;
    VkCommandPool commandPool; VkCommandBufferLevel level;
    uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct {
    VkStructureType sType; const void *pNext; VkFlags flags;
    const void *pInheritanceInfo;
} VkCommandBufferBeginInfo;

typedef struct {
    VkStructureType sType; const void *pNext;
    VkRenderPass renderPass; VkFramebuffer framebuffer; VkRect2D renderArea;
    uint32_t clearValueCount; const VkClearValue *pClearValues;
} VkRenderPassBeginInfo;

typedef struct {
    VkStructureType sType; const void *pNext;
    uint32_t waitSemaphoreCount; const void *pWaitSemaphores;
    const VkFlags *pWaitDstStageMask;
    uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount; const void *pSignalSemaphores;
} VkSubmitInfo;

/* ----------------------------------------------------------- the target */

/* Vulkan gets its images from a swapchain tied to a window system.  There is no
 * WSI here, so instead an image is bound directly to a surface the program
 * already owns.  Everything downstream of this is ordinary Vulkan. */
VkResult vkBindImageToSurface(VkImage image, void *colour_surface);

/* ------------------------------------------------------------ the calls */

VkResult vkCreateInstance(const VkInstanceCreateInfo *info, const void *alloc, VkInstance *out);
void     vkDestroyInstance(VkInstance instance, const void *alloc);
VkResult vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count, VkPhysicalDevice *out);
void     vkGetPhysicalDeviceProperties(VkPhysicalDevice dev, VkPhysicalDeviceProperties *out);
void     vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice dev, uint32_t *count,
                                                  VkQueueFamilyProperties *out);
void     vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice dev,
                                             VkPhysicalDeviceMemoryProperties *out);
VkResult vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *info,
                        const void *alloc, VkDevice *out);
void     vkDestroyDevice(VkDevice device, const void *alloc);
void     vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out);

VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *info, const void *a, VkBuffer *out);
void     vkDestroyBuffer(VkDevice d, VkBuffer b, const void *a);
void     vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *out);
VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *info, const void *a,
                          VkDeviceMemory *out);
void     vkFreeMemory(VkDevice d, VkDeviceMemory m, const void *a);
VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset);
VkResult vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize size,
                     VkFlags flags, void **out);
void     vkUnmapMemory(VkDevice d, VkDeviceMemory m);

VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo *info, const void *a, VkImage *out);
void     vkDestroyImage(VkDevice d, VkImage i, const void *a);
VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *info, const void *a,
                           VkImageView *out);
void     vkDestroyImageView(VkDevice d, VkImageView v, const void *a);

VkResult vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *info, const void *a,
                            VkRenderPass *out);
void     vkDestroyRenderPass(VkDevice d, VkRenderPass r, const void *a);
VkResult vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *info, const void *a,
                             VkFramebuffer *out);
void     vkDestroyFramebuffer(VkDevice d, VkFramebuffer f, const void *a);

VkResult vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *info, const void *a,
                              VkShaderModule *out);
void     vkDestroyShaderModule(VkDevice d, VkShaderModule m, const void *a);
VkResult vkCreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *info,
                                const void *a, VkPipelineLayout *out);
void     vkDestroyPipelineLayout(VkDevice d, VkPipelineLayout l, const void *a);
VkResult vkCreateGraphicsPipelines(VkDevice d, void *cache, uint32_t count,
                                   const VkGraphicsPipelineCreateInfo *infos,
                                   const void *a, VkPipeline *out);
void     vkDestroyPipeline(VkDevice d, VkPipeline p, const void *a);

VkResult vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *info, const void *a,
                             VkCommandPool *out);
void     vkDestroyCommandPool(VkDevice d, VkCommandPool p, const void *a);
VkResult vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *info,
                                  VkCommandBuffer *out);
void     vkFreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t count,
                              const VkCommandBuffer *bufs);

VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info);
VkResult vkEndCommandBuffer(VkCommandBuffer cb);
VkResult vkResetCommandBuffer(VkCommandBuffer cb, VkFlags flags);

void vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info,
                          VkSubpassContents contents);
void vkCmdEndRenderPass(VkCommandBuffer cb);
void vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipeline p);
void vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                      const VkViewport *viewports);
void vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count,
                     const VkRect2D *scissors);
void vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                            const VkBuffer *buffers, const VkDeviceSize *offsets);
void vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset,
                          VkIndexType type);
void vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount, uint32_t instanceCount,
               uint32_t firstVertex, uint32_t firstInstance);
void vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount,
                      uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
/* Not part of Vulkan: the fixed-function pipeline has to be told the matrix a
 * vertex shader would otherwise apply.  Named so it cannot be mistaken for a
 * standard entry point. */
void vkCmdSetTransformKESTREL(VkCommandBuffer cb, const float *matrix4x4);

VkResult vkQueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo *submits, VkFence fence);
VkResult vkQueueWaitIdle(VkQueue q);
VkResult vkDeviceWaitIdle(VkDevice d);

#endif
