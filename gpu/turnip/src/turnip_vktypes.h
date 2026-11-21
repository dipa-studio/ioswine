/*
 * turnip_vktypes.h: minimal Vulkan types for the Turnip helper wire
 *
 *
 * The lsk is freestanding and can never include <vulkan/vulkan.h>,
 * so the stubs use these types. Every struct here is laid out to
 * match the official Vulkan struct of the same name field for field
 * (Vulkan 1.3, header 1.3.275): same member order, same member
 * types, so the same size and alignment on LP64. The helper
 * (gpu-helper.c, which includes the real <vulkan/vulkan.h>)
 * static-asserts sizeof() equality for every struct it memcpys, so
 * a layout drift fails the helper build instead of silently
 * corrupting a call. The host test does the same check.
 *
 * Handles are opaque pointers, as in Vulkan (VK_DEFINE_HANDLE).
 * On the wire a handle is a u64 id assigned by the helper; the
 * client never dereferences it. VkResult values and the structure
 * type constants used here are the official ones.
 *
 * Only the types the 38 vkshim entries need are defined. Deliberate
 * omissions (documented in README.md): pNext chains are not
 * carried (must be NULL), VkPipelineShaderStageCreateInfo
 * pSpecializationInfo is not carried (must be NULL),
 * VkCommandBufferBeginInfo pInheritanceInfo is not carried (must be
 * NULL, primary command buffers only), and no custom VkAllocationCallbacks
 * are supported (pAllocator must be NULL).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Scalars and handles                                                  */
/* ------------------------------------------------------------------ */

typedef int32_t tw_VkResult;
typedef uint32_t tw_VkBool32;
typedef uint64_t tw_VkDeviceSize;
typedef uint32_t tw_VkFlags;
typedef uint32_t tw_VkStructureType;
typedef uint32_t tw_VkDeviceAddress;

#define TW_VK_TRUE  1u
#define TW_VK_FALSE 0u

/* Opaque handles (pointer-sized, never dereferenced on the client). */
#define TW_VK_DEFINE_HANDLE(name) typedef struct tw_##name##_t *tw_##name
TW_VK_DEFINE_HANDLE(VkInstance);
TW_VK_DEFINE_HANDLE(VkPhysicalDevice);
TW_VK_DEFINE_HANDLE(VkDevice);
TW_VK_DEFINE_HANDLE(VkQueue);
TW_VK_DEFINE_HANDLE(VkBuffer);
TW_VK_DEFINE_HANDLE(VkImage);
TW_VK_DEFINE_HANDLE(VkDeviceMemory);
TW_VK_DEFINE_HANDLE(VkCommandPool);
TW_VK_DEFINE_HANDLE(VkCommandBuffer);
TW_VK_DEFINE_HANDLE(VkRenderPass);
TW_VK_DEFINE_HANDLE(VkFramebuffer);
TW_VK_DEFINE_HANDLE(VkShaderModule);
TW_VK_DEFINE_HANDLE(VkPipeline);
TW_VK_DEFINE_HANDLE(VkPipelineCache);
TW_VK_DEFINE_HANDLE(VkSemaphore);
TW_VK_DEFINE_HANDLE(VkFence);
TW_VK_DEFINE_HANDLE(VkPipelineLayout);
TW_VK_DEFINE_HANDLE(VkImageView);
TW_VK_DEFINE_HANDLE(VkSampler);
TW_VK_DEFINE_HANDLE(VkDescriptorSetLayout);

/* ------------------------------------------------------------------ */
/* VkResult values used on the wire                                     */
/* ------------------------------------------------------------------ */

#define TW_VK_SUCCESS                         0
#define TW_VK_NOT_READY                       1
#define TW_VK_TIMEOUT                         2
#define TW_VK_EVENT_SET                       3
#define TW_VK_EVENT_RESET                     4
#define TW_VK_INCOMPLETE                      5
#define TW_VK_ERROR_OUT_OF_HOST_MEMORY       (-1)
#define TW_VK_ERROR_OUT_OF_DEVICE_MEMORY     (-2)
#define TW_VK_ERROR_INITIALIZATION_FAILED    (-3)
#define TW_VK_ERROR_DEVICE_LOST              (-4)
#define TW_VK_ERROR_MEMORY_MAP_FAILED        (-5)
#define TW_VK_ERROR_LAYER_NOT_PRESENT        (-6)
#define TW_VK_ERROR_EXTENSION_NOT_PRESENT    (-7)
#define TW_VK_ERROR_FEATURE_NOT_PRESENT      (-8)
#define TW_VK_ERROR_INCOMPATIBLE_DRIVER      (-9)
#define TW_VK_ERROR_TOO_MANY_OBJECTS         (-10)
#define TW_VK_ERROR_FORMAT_NOT_SUPPORTED     (-11)
#define TW_VK_ERROR_FRAGMENTED_POOL          (-12)
#define TW_VK_ERROR_UNKNOWN                  (-13)

/* ------------------------------------------------------------------ */
/* Structure types used here                                            */
/* ------------------------------------------------------------------ */

#define TW_VK_STRUCTURE_TYPE_APPLICATION_INFO                        0
#define TW_VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO                    1
#define TW_VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO                2
#define TW_VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO                      3
#define TW_VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO                    5
#define TW_VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO                     12
#define TW_VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO                      14
#define TW_VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO               39
#define TW_VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO           40
#define TW_VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO              42
#define TW_VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO                 43
#define TW_VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO                38
#define TW_VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO                37
#define TW_VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO              16
#define TW_VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO          28
#define TW_VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO      18
#define TW_VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO 19
#define TW_VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO 20
#define TW_VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO 21
#define TW_VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO    22
#define TW_VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO 23
#define TW_VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO 24
#define TW_VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO 25
#define TW_VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO 26
#define TW_VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO     27
#define TW_VK_STRUCTURE_TYPE_SUBMIT_INFO                             4

/* ------------------------------------------------------------------ */
/* Common enums (u32, official values)                                  */
/* ------------------------------------------------------------------ */

/* VkPhysicalDeviceType */
#define TW_VK_PHYSICAL_DEVICE_TYPE_OTHER          0
#define TW_VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU 1
#define TW_VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   2
#define TW_VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    3
#define TW_VK_PHYSICAL_DEVICE_TYPE_CPU            4

/* VkFormat (only the formats the clear-color pass uses) */
#define TW_VK_FORMAT_UNDEFINED                    0
#define TW_VK_FORMAT_R8G8B8A8_UNORM               37
#define TW_VK_FORMAT_B8G8R8A8_UNORM               44
#define TW_VK_FORMAT_B8G8R8A8_SRGB                50
#define TW_VK_FORMAT_D32_SFLOAT                   126

/* VkImageType */
#define TW_VK_IMAGE_TYPE_1D 0
#define TW_VK_IMAGE_TYPE_2D 1
#define TW_VK_IMAGE_TYPE_3D 2

/* VkImageTiling */
#define TW_VK_IMAGE_TILING_OPTIMAL 0
#define TW_VK_IMAGE_TILING_LINEAR  1

/* VkImageLayout */
#define TW_VK_IMAGE_LAYOUT_UNDEFINED                        0
#define TW_VK_IMAGE_LAYOUT_GENERAL                          1
#define TW_VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL         2
#define TW_VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL 3
#define TW_VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL         5
#define TW_VK_IMAGE_LAYOUT_PRESENT_SRC_KHR                  1000001002u

/* VkAttachmentLoadOp / VkAttachmentStoreOp */
#define TW_VK_ATTACHMENT_LOAD_OP_LOAD       0
#define TW_VK_ATTACHMENT_LOAD_OP_CLEAR      1
#define TW_VK_ATTACHMENT_LOAD_OP_DONT_CARE  2
#define TW_VK_ATTACHMENT_STORE_OP_STORE     0
#define TW_VK_ATTACHMENT_STORE_OP_DONT_CARE 1

/* VkPipelineBindPoint */
#define TW_VK_PIPELINE_BIND_POINT_GRAPHICS 0
#define TW_VK_PIPELINE_BIND_POINT_COMPUTE  1

/* VkShaderStageFlagBits */
#define TW_VK_SHADER_STAGE_VERTEX_BIT   0x00000001u
#define TW_VK_SHADER_STAGE_FRAGMENT_BIT 0x00000010u

/* VkPrimitiveTopology */
#define TW_VK_PRIMITIVE_TOPOLOGY_POINT_LIST     0
#define TW_VK_PRIMITIVE_TOPOLOGY_LINE_LIST      1
#define TW_VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST  3

/* VkPolygonMode */
#define TW_VK_POLYGON_MODE_FILL  0
#define TW_VK_POLYGON_MODE_LINE  1
#define TW_VK_POLYGON_MODE_POINT 2

/* VkCullModeFlagBits */
#define TW_VK_CULL_MODE_NONE               0
#define TW_VK_CULL_MODE_FRONT_BIT          0x00000001u
#define TW_VK_CULL_MODE_BACK_BIT           0x00000002u
#define TW_VK_CULL_MODE_FRONT_AND_BACK     0x00000003u

/* VkFrontFace */
#define TW_VK_FRONT_FACE_COUNTER_CLOCKWISE 0
#define TW_VK_FRONT_FACE_CLOCKWISE         1

/* VkCompareOp */
#define TW_VK_COMPARE_OP_NEVER            0
#define TW_VK_COMPARE_OP_LESS             1
#define TW_VK_COMPARE_OP_EQUAL            2
#define TW_VK_COMPARE_OP_LESS_OR_EQUAL    3
#define TW_VK_COMPARE_OP_GREATER          4
#define TW_VK_COMPARE_OP_NOT_EQUAL        5
#define TW_VK_COMPARE_OP_GREATER_OR_EQUAL 6
#define TW_VK_COMPARE_OP_ALWAYS           7

/* VkStencilOp */
#define TW_VK_STENCIL_OP_KEEP                0
#define TW_VK_STENCIL_OP_ZERO                1
#define TW_VK_STENCIL_OP_REPLACE             2
#define TW_VK_STENCIL_OP_INCREMENT_AND_CLAMP 3
#define TW_VK_STENCIL_OP_DECREMENT_AND_CLAMP 4
#define TW_VK_STENCIL_OP_INVERT              5
#define TW_VK_STENCIL_OP_INCREMENT_AND_WRAP  6
#define TW_VK_STENCIL_OP_DECREMENT_AND_WRAP  7

/* VkLogicOp */
#define TW_VK_LOGIC_OP_CLEAR   0
#define TW_VK_LOGIC_OP_COPY    3
#define TW_VK_LOGIC_OP_NO_OP   5

/* VkBlendFactor */
#define TW_VK_BLEND_FACTOR_ZERO                     0
#define TW_VK_BLEND_FACTOR_ONE                      1
#define TW_VK_BLEND_FACTOR_SRC_ALPHA                6
#define TW_VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA      7

/* VkBlendOp */
#define TW_VK_BLEND_OP_ADD 0

/* VkColorComponentFlagBits */
#define TW_VK_COLOR_COMPONENT_R_BIT 0x00000001u
#define TW_VK_COLOR_COMPONENT_G_BIT 0x00000002u
#define TW_VK_COLOR_COMPONENT_B_BIT 0x00000004u
#define TW_VK_COLOR_COMPONENT_A_BIT 0x00000008u

/* VkDynamicState */
#define TW_VK_DYNAMIC_STATE_VIEWPORT 0
#define TW_VK_DYNAMIC_STATE_SCISSOR  1

/* VkCommandBufferLevel */
#define TW_VK_COMMAND_BUFFER_LEVEL_PRIMARY   0
#define TW_VK_COMMAND_BUFFER_LEVEL_SECONDARY 1

/* VkCommandBufferUsageFlagBits */
#define TW_VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x00000001u

/* VkSampleCountFlagBits */
#define TW_VK_SAMPLE_COUNT_1_BIT 0x00000001u

/* VkBufferUsageFlagBits */
#define TW_VK_BUFFER_USAGE_TRANSFER_SRC_BIT            0x00000001u
#define TW_VK_BUFFER_USAGE_TRANSFER_DST_BIT            0x00000002u
#define TW_VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT          0x00000010u
#define TW_VK_BUFFER_USAGE_VERTEX_BUFFER_BIT           0x00000080u
#define TW_VK_BUFFER_USAGE_INDEX_BUFFER_BIT            0x00000040u

/* VkImageUsageFlagBits */
#define TW_VK_IMAGE_USAGE_TRANSFER_SRC_BIT             0x00000001u
#define TW_VK_IMAGE_USAGE_TRANSFER_DST_BIT             0x00000002u
#define TW_VK_IMAGE_USAGE_SAMPLED_BIT                  0x00000004u
#define TW_VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT         0x00000010u
#define TW_VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT 0x00000020u

/* VkMemoryPropertyFlagBits */
#define TW_VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT   0x00000001u
#define TW_VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT   0x00000002u
#define TW_VK_MEMORY_PROPERTY_HOST_COHERENT_BIT  0x00000004u

/* VkSharingMode */
#define TW_VK_SHARING_MODE_EXCLUSIVE  0
#define TW_VK_SHARING_MODE_CONCURRENT 1

/* VkQueueFlagBits */
#define TW_VK_QUEUE_GRAPHICS_BIT 0x00000001u
#define TW_VK_QUEUE_COMPUTE_BIT  0x00000002u

/* VkImageAspectFlagBits */
#define TW_VK_IMAGE_ASPECT_COLOR_BIT   0x00000001u
#define TW_VK_IMAGE_ASPECT_DEPTH_BIT   0x00000002u
#define TW_VK_IMAGE_ASPECT_STENCIL_BIT 0x00000004u

/* VkSubpassContents */
#define TW_VK_SUBPASS_CONTENTS_INLINE                   0
#define TW_VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS 1

/* API version helpers (same encoding as VK_MAKE_API_VERSION). */
#define TW_VK_MAKE_API_VERSION(variant, major, minor, patch) \
    ((((uint32_t)(variant)) << 29) | (((uint32_t)(major)) << 22) | \
     (((uint32_t)(minor)) << 12) | ((uint32_t)(patch)))
#define TW_VK_API_VERSION_1_0 TW_VK_MAKE_API_VERSION(0, 1, 0, 0)
#define TW_VK_API_VERSION_1_1 TW_VK_MAKE_API_VERSION(0, 1, 1, 0)
#define TW_VK_API_VERSION_1_2 TW_VK_MAKE_API_VERSION(0, 1, 2, 0)
#define TW_VK_API_VERSION_1_3 TW_VK_MAKE_API_VERSION(0, 1, 3, 0)

/* ------------------------------------------------------------------ */
/* Geometry                                                             */
/* ------------------------------------------------------------------ */

typedef struct tw_VkExtent2D {
    uint32_t width;
    uint32_t height;
} tw_VkExtent2D;

typedef struct tw_VkExtent3D {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} tw_VkExtent3D;

typedef struct tw_VkOffset2D {
    int32_t x;
    int32_t y;
} tw_VkOffset2D;

typedef struct tw_VkOffset3D {
    int32_t x;
    int32_t y;
    int32_t z;
} tw_VkOffset3D;

typedef struct tw_VkRect2D {
    tw_VkOffset2D offset;
    tw_VkExtent2D extent;
} tw_VkRect2D;

typedef struct tw_VkViewport {
    float x;
    float y;
    float width;
    float height;
    float minDepth;
    float maxDepth;
} tw_VkViewport;

/* ------------------------------------------------------------------ */
/* Instance                                                             */
/* ------------------------------------------------------------------ */

typedef struct tw_VkApplicationInfo {
    tw_VkStructureType sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} tw_VkApplicationInfo;

typedef struct tw_VkInstanceCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    const tw_VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} tw_VkInstanceCreateInfo;

/* ------------------------------------------------------------------ */
/* Physical device queries                                              */
/* ------------------------------------------------------------------ */

typedef struct tw_VkQueueFamilyProperties {
    tw_VkFlags queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    tw_VkExtent3D minImageTransferGranularity;
} tw_VkQueueFamilyProperties;

typedef struct tw_VkMemoryType {
    tw_VkFlags propertyFlags;
    uint32_t heapIndex;
} tw_VkMemoryType;

typedef struct tw_VkMemoryHeap {
    tw_VkDeviceSize size;
    tw_VkFlags flags;
} tw_VkMemoryHeap;

#define TW_VK_MAX_MEMORY_TYPES 32
#define TW_VK_MAX_MEMORY_HEAPS 16

typedef struct tw_VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    tw_VkMemoryType memoryTypes[TW_VK_MAX_MEMORY_TYPES];
    uint32_t memoryHeapCount;
    tw_VkMemoryHeap memoryHeaps[TW_VK_MAX_MEMORY_HEAPS];
} tw_VkPhysicalDeviceMemoryProperties;

/* ------------------------------------------------------------------ */
/* Device                                                               */
/* ------------------------------------------------------------------ */

typedef struct tw_VkDeviceQueueCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} tw_VkDeviceQueueCreateInfo;

/* All 55 members, official order (Vulkan 1.3). The wire carries
 * this struct as a 220-byte blob; the helper memcpys it into the
 * real VkPhysicalDeviceFeatures. */
typedef struct tw_VkPhysicalDeviceFeatures {
    tw_VkBool32 robustBufferAccess;
    tw_VkBool32 fullDrawIndexUint32;
    tw_VkBool32 imageCubeArray;
    tw_VkBool32 independentBlend;
    tw_VkBool32 geometryShader;
    tw_VkBool32 tessellationShader;
    tw_VkBool32 sampleRateShading;
    tw_VkBool32 dualSrcBlend;
    tw_VkBool32 logicOp;
    tw_VkBool32 multiDrawIndirect;
    tw_VkBool32 drawIndirectFirstInstance;
    tw_VkBool32 depthClamp;
    tw_VkBool32 depthBiasClamp;
    tw_VkBool32 fillModeNonSolid;
    tw_VkBool32 depthBounds;
    tw_VkBool32 wideLines;
    tw_VkBool32 largePoints;
    tw_VkBool32 alphaToOne;
    tw_VkBool32 multiViewport;
    tw_VkBool32 samplerAnisotropy;
    tw_VkBool32 textureCompressionETC2;
    tw_VkBool32 textureCompressionASTC_LDR;
    tw_VkBool32 textureCompressionBC;
    tw_VkBool32 occlusionQueryPrecise;
    tw_VkBool32 pipelineStatisticsQuery;
    tw_VkBool32 vertexPipelineStoresAndAtomics;
    tw_VkBool32 fragmentStoresAndAtomics;
    tw_VkBool32 shaderTessellationAndGeometryPointSize;
    tw_VkBool32 shaderImageGatherExtended;
    tw_VkBool32 shaderStorageImageExtendedFormats;
    tw_VkBool32 shaderStorageImageMultisample;
    tw_VkBool32 shaderStorageImageReadWithoutFormat;
    tw_VkBool32 shaderStorageImageWriteWithoutFormat;
    tw_VkBool32 shaderUniformBufferArrayDynamicIndexing;
    tw_VkBool32 shaderSampledImageArrayDynamicIndexing;
    tw_VkBool32 shaderStorageBufferArrayDynamicIndexing;
    tw_VkBool32 shaderStorageImageArrayDynamicIndexing;
    tw_VkBool32 shaderClipDistance;
    tw_VkBool32 shaderCullDistance;
    tw_VkBool32 shaderFloat64;
    tw_VkBool32 shaderInt64;
    tw_VkBool32 shaderInt16;
    tw_VkBool32 shaderResourceResidency;
    tw_VkBool32 shaderResourceMinLod;
    tw_VkBool32 sparseBinding;
    tw_VkBool32 sparseResidencyBuffer;
    tw_VkBool32 sparseResidencyImage2D;
    tw_VkBool32 sparseResidencyImage3D;
    tw_VkBool32 sparseResidency2Samples;
    tw_VkBool32 sparseResidency4Samples;
    tw_VkBool32 sparseResidency8Samples;
    tw_VkBool32 sparseResidency16Samples;
    tw_VkBool32 sparseResidencyAliased;
    tw_VkBool32 variableMultisampleRate;
    tw_VkBool32 inheritedQueries;
} tw_VkPhysicalDeviceFeatures;

typedef struct tw_VkDeviceCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t queueCreateInfoCount;
    const tw_VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const tw_VkPhysicalDeviceFeatures *pEnabledFeatures;
} tw_VkDeviceCreateInfo;

/* ------------------------------------------------------------------ */
/* Memory                                                               */
/* ------------------------------------------------------------------ */

typedef struct tw_VkMemoryAllocateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
} tw_VkMemoryAllocateInfo;

typedef struct tw_VkMemoryRequirements {
    tw_VkDeviceSize size;
    tw_VkDeviceSize alignment;
    uint32_t memoryTypeBits;
} tw_VkMemoryRequirements;

/* ------------------------------------------------------------------ */
/* Buffer / image                                                       */
/* ------------------------------------------------------------------ */

typedef struct tw_VkBufferCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    tw_VkDeviceSize size;
    tw_VkFlags usage;
    uint32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
} tw_VkBufferCreateInfo;

typedef struct tw_VkImageCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t imageType;
    uint32_t format;
    tw_VkExtent3D extent;
    uint32_t mipLevels;
    uint32_t arrayLayers;
    uint32_t samples;
    uint32_t tiling;
    tw_VkFlags usage;
    uint32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    uint32_t initialLayout;
} tw_VkImageCreateInfo;

/* ------------------------------------------------------------------ */
/* Command pool / buffers                                               */
/* ------------------------------------------------------------------ */

typedef struct tw_VkCommandPoolCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t queueFamilyIndex;
} tw_VkCommandPoolCreateInfo;

typedef struct tw_VkCommandBufferAllocateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkCommandPool commandPool;
    uint32_t level;
    uint32_t commandBufferCount;
} tw_VkCommandBufferAllocateInfo;

typedef struct tw_VkCommandBufferBeginInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    const void *pInheritanceInfo;
} tw_VkCommandBufferBeginInfo;

/* ------------------------------------------------------------------ */
/* Render pass                                                          */
/* ------------------------------------------------------------------ */

typedef struct tw_VkAttachmentDescription {
    tw_VkFlags flags;
    uint32_t format;
    uint32_t samples;
    uint32_t loadOp;
    uint32_t storeOp;
    uint32_t stencilLoadOp;
    uint32_t stencilStoreOp;
    uint32_t initialLayout;
    uint32_t finalLayout;
} tw_VkAttachmentDescription;

typedef struct tw_VkAttachmentReference {
    uint32_t attachment;
    uint32_t layout;
} tw_VkAttachmentReference;

typedef struct tw_VkSubpassDescription {
    tw_VkFlags flags;
    uint32_t pipelineBindPoint;
    uint32_t inputAttachmentCount;
    const tw_VkAttachmentReference *pInputAttachments;
    uint32_t colorAttachmentCount;
    const tw_VkAttachmentReference *pColorAttachments;
    const tw_VkAttachmentReference *pResolveAttachments;
    const tw_VkAttachmentReference *pDepthStencilAttachment;
    uint32_t preserveAttachmentCount;
    const uint32_t *pPreserveAttachments;
} tw_VkSubpassDescription;

typedef struct tw_VkSubpassDependency {
    uint32_t srcSubpass;
    uint32_t dstSubpass;
    uint32_t srcStageMask;
    uint32_t dstStageMask;
    uint32_t srcAccessMask;
    uint32_t dstAccessMask;
    tw_VkFlags dependencyFlags;
} tw_VkSubpassDependency;

typedef struct tw_VkRenderPassCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t attachmentCount;
    const tw_VkAttachmentDescription *pAttachments;
    uint32_t subpassCount;
    const tw_VkSubpassDescription *pSubpasses;
    uint32_t dependencyCount;
    const tw_VkSubpassDependency *pDependencies;
} tw_VkRenderPassCreateInfo;

typedef struct tw_VkFramebufferCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    tw_VkRenderPass renderPass;
    uint32_t attachmentCount;
    const tw_VkImageView *pAttachments;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
} tw_VkFramebufferCreateInfo;

/* ------------------------------------------------------------------ */
/* Shader module                                                        */
/* ------------------------------------------------------------------ */

typedef struct tw_VkShaderModuleCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    size_t codeSize;
    const uint32_t *pCode;
} tw_VkShaderModuleCreateInfo;

/* ------------------------------------------------------------------ */
/* Graphics pipeline                                                    */
/* ------------------------------------------------------------------ */

typedef struct tw_VkPipelineShaderStageCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t stage;
    tw_VkShaderModule module;
    const char *pName;
    const void *pSpecializationInfo;
} tw_VkPipelineShaderStageCreateInfo;

typedef struct tw_VkVertexInputBindingDescription {
    uint32_t binding;
    uint32_t stride;
    uint32_t inputRate;
} tw_VkVertexInputBindingDescription;

typedef struct tw_VkVertexInputAttributeDescription {
    uint32_t location;
    uint32_t binding;
    uint32_t format;
    uint32_t offset;
} tw_VkVertexInputAttributeDescription;

typedef struct tw_VkPipelineVertexInputStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t vertexBindingDescriptionCount;
    const tw_VkVertexInputBindingDescription *pVertexBindingDescriptions;
    uint32_t vertexAttributeDescriptionCount;
    const tw_VkVertexInputAttributeDescription *pVertexAttributeDescriptions;
} tw_VkPipelineVertexInputStateCreateInfo;

typedef struct tw_VkPipelineInputAssemblyStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t topology;
    tw_VkBool32 primitiveRestartEnable;
} tw_VkPipelineInputAssemblyStateCreateInfo;

typedef struct tw_VkPipelineTessellationStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t patchControlPoints;
} tw_VkPipelineTessellationStateCreateInfo;

typedef struct tw_VkPipelineViewportStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t viewportCount;
    const tw_VkViewport *pViewports;
    uint32_t scissorCount;
    const tw_VkRect2D *pScissors;
} tw_VkPipelineViewportStateCreateInfo;

typedef struct tw_VkPipelineRasterizationStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    tw_VkBool32 depthClampEnable;
    tw_VkBool32 rasterizerDiscardEnable;
    uint32_t polygonMode;
    uint32_t cullMode;
    uint32_t frontFace;
    tw_VkBool32 depthBiasEnable;
    float depthBiasConstantFactor;
    float depthBiasClamp;
    float depthBiasSlopeFactor;
    float lineWidth;
} tw_VkPipelineRasterizationStateCreateInfo;

typedef struct tw_VkPipelineMultisampleStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t rasterizationSamples;
    tw_VkBool32 sampleShadingEnable;
    float minSampleShading;
    const uint32_t *pSampleMask;
    tw_VkBool32 alphaToCoverageEnable;
    tw_VkBool32 alphaToOneEnable;
} tw_VkPipelineMultisampleStateCreateInfo;

typedef struct tw_VkStencilOpState {
    uint32_t failOp;
    uint32_t passOp;
    uint32_t depthFailOp;
    uint32_t compareOp;
    uint32_t compareMask;
    uint32_t writeMask;
    uint32_t reference;
} tw_VkStencilOpState;

typedef struct tw_VkPipelineDepthStencilStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    tw_VkBool32 depthTestEnable;
    tw_VkBool32 depthWriteEnable;
    uint32_t depthCompareOp;
    tw_VkBool32 depthBoundsTestEnable;
    tw_VkBool32 stencilTestEnable;
    tw_VkStencilOpState front;
    tw_VkStencilOpState back;
    float minDepthBounds;
    float maxDepthBounds;
} tw_VkPipelineDepthStencilStateCreateInfo;

typedef struct tw_VkPipelineColorBlendAttachmentState {
    tw_VkBool32 blendEnable;
    uint32_t srcColorBlendFactor;
    uint32_t dstColorBlendFactor;
    uint32_t colorBlendOp;
    uint32_t srcAlphaBlendFactor;
    uint32_t dstAlphaBlendFactor;
    uint32_t alphaBlendOp;
    uint32_t colorWriteMask;
} tw_VkPipelineColorBlendAttachmentState;

typedef struct tw_VkPipelineColorBlendStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    tw_VkBool32 logicOpEnable;
    uint32_t logicOp;
    uint32_t attachmentCount;
    const tw_VkPipelineColorBlendAttachmentState *pAttachments;
    float blendConstants[4];
} tw_VkPipelineColorBlendStateCreateInfo;

typedef struct tw_VkPipelineDynamicStateCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t dynamicStateCount;
    const uint32_t *pDynamicStates;
} tw_VkPipelineDynamicStateCreateInfo;

typedef struct tw_VkGraphicsPipelineCreateInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkFlags flags;
    uint32_t stageCount;
    const tw_VkPipelineShaderStageCreateInfo *pStages;
    const tw_VkPipelineVertexInputStateCreateInfo *pVertexInputState;
    const tw_VkPipelineInputAssemblyStateCreateInfo *pInputAssemblyState;
    const tw_VkPipelineTessellationStateCreateInfo *pTessellationState;
    const tw_VkPipelineViewportStateCreateInfo *pViewportState;
    const tw_VkPipelineRasterizationStateCreateInfo *pRasterizationState;
    const tw_VkPipelineMultisampleStateCreateInfo *pMultisampleState;
    const tw_VkPipelineDepthStencilStateCreateInfo *pDepthStencilState;
    const tw_VkPipelineColorBlendStateCreateInfo *pColorBlendState;
    const tw_VkPipelineDynamicStateCreateInfo *pDynamicState;
    tw_VkPipelineLayout layout;
    tw_VkRenderPass renderPass;
    uint32_t subpass;
    tw_VkPipeline basePipelineHandle;
    int32_t basePipelineIndex;
} tw_VkGraphicsPipelineCreateInfo;

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

/* A clear value: 16 bytes, either four floats/ints or depth+stencil.
 * The wire carries it as raw bytes; the helper memcpys it into the
 * real VkClearValue. */
typedef union tw_VkClearValue {
    float float32[4];
    int32_t int32[4];
    uint32_t uint32[4];
    struct {
        float depth;
        uint32_t stencil;
    } depthStencil;
} tw_VkClearValue;

typedef struct tw_VkRenderPassBeginInfo {
    tw_VkStructureType sType;
    const void *pNext;
    tw_VkRenderPass renderPass;
    tw_VkFramebuffer framebuffer;
    tw_VkRect2D renderArea;
    uint32_t clearValueCount;
    const tw_VkClearValue *pClearValues;
} tw_VkRenderPassBeginInfo;

typedef struct tw_VkClearAttachment {
    uint32_t aspectMask;
    uint32_t colorAttachment;
    tw_VkClearValue clearValue;
} tw_VkClearAttachment;

typedef struct tw_VkClearRect {
    tw_VkRect2D rect;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} tw_VkClearRect;

typedef struct tw_VkSubmitInfo {
    tw_VkStructureType sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const tw_VkSemaphore *pWaitSemaphores;
    const uint32_t *pWaitDstStageMask;
    uint32_t commandBufferCount;
    const tw_VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const tw_VkSemaphore *pSignalSemaphores;
} tw_VkSubmitInfo;

#ifdef __cplusplus
}
#endif
