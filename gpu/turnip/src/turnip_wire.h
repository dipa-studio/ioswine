/*
 * turnip_wire.h: shared wire protocol between the lsk-side Turnip
 * stubs (turnip.c) and the GPU helper process (gpu-helper.c).
 *
 *
 * Transport: one Unix stream socket at a filesystem path given to
 * turnip_init(). Synchronous request/response: the client sends one
 * request, the helper executes the real Vulkan call and sends one
 * response. Little-endian, same architecture on both ends (aarch64
 * on device, x86_64 in the host test).
 *
 * Framing: a fixed header plus a payload. The payload is one
 * fixed-size request/response struct (defined below) followed by
 * variable blobs in the order the struct documents. Handles on the
 * wire are u64 ids assigned by the helper; the client stores the id
 * as the value of its opaque handle pointer and never dereferences
 * it. 0 is VK_NULL_HANDLE.
 *
 * Struct blobs: create-info structs are carried as raw bytes of the
 * tw_ struct (layout-identical to the official Vulkan struct, see
 * turnip_vktypes.h). Pointer fields in a wire copy are 0; the
 * helper rebuilds them from the blobs that follow. pNext must be
 * NULL (the stub rejects a non-NULL pNext with
 * TW_VK_ERROR_FEATURE_NOT_PRESENT before any round-trip).
 * pAllocator must be NULL (custom allocation callbacks cannot be
 * marshaled; the stub sends 0).
 *
 * Limits (documented caps, enforced by the stub): at most 8
 * pipelines per vkCreateGraphicsPipelines call, at most 64 layer /
 * extension names, at most 64 queue create infos, at most 16
 * subpasses, at most 32 attachments, at most 16 clear values,
 * at most 8 submits per vkQueueSubmit. Payloads are capped at
 * TURNIP_MAX_PAYLOAD bytes.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "turnip_vktypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURNIP_WIRE_MAGIC   0x4E525554u /* "TURN" */
#define TURNIP_WIRE_VERSION 1
#define TURNIP_MAX_PAYLOAD  65536
#define TURNIP_MAX_PATH     108

/* Wire handle: a u64 id. The client holds it as the value of an
 * opaque handle pointer. */
typedef uint64_t tw_handle_t;

/* Request header, sent by the client. */
typedef struct tw_req_hdr {
    uint32_t magic;       /* TURNIP_WIRE_MAGIC */
    uint32_t op;          /* tw_turnip_op */
    uint32_t seq;         /* client sequence, echoed in the response */
    uint32_t payload_len; /* bytes after this header */
} tw_req_hdr;

/* Response header, sent by the helper. result is the VkResult of
 * the call (0 for a void call that executed; a negative error when
 * the call could not run, e.g. unknown handle id). */
typedef struct tw_resp_hdr {
    uint32_t magic;       /* TURNIP_WIRE_MAGIC */
    uint32_t op;          /* echoes the request op */
    uint32_t seq;         /* echoes the request seq */
    int32_t result;       /* VkResult */
    uint32_t payload_len; /* bytes after this header */
    uint32_t reserved;    /* padding: keeps the header 24 bytes so the
                             payload that follows is 8-byte aligned
                             (the response structs carry u64 fields) */
} tw_resp_hdr;

/* Both headers must keep the payload that follows them 8-byte
 * aligned: the payload structs carry u64 fields, and a misaligned
 * payload is UB on arm64 (and flagged by UBSan on the host). */
_Static_assert(sizeof(tw_req_hdr) % 8 == 0,
               "tw_req_hdr must be a multiple of 8 bytes");
_Static_assert(sizeof(tw_resp_hdr) % 8 == 0,
               "tw_resp_hdr must be a multiple of 8 bytes");

enum tw_turnip_op {
    TURNIP_OP_PING = 0,
    TURNIP_OP_VK_CREATE_INSTANCE = 1,
    TURNIP_OP_VK_ENUMERATE_PHYSICAL_DEVICES = 2,
    TURNIP_OP_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES = 3,
    TURNIP_OP_VK_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES = 4,
    TURNIP_OP_VK_CREATE_DEVICE = 5,
    TURNIP_OP_VK_GET_DEVICE_QUEUE = 6,
    TURNIP_OP_VK_DEVICE_WAIT_IDLE = 7,
    TURNIP_OP_VK_ALLOCATE_MEMORY = 8,
    TURNIP_OP_VK_FREE_MEMORY = 9,
    TURNIP_OP_VK_GET_BUFFER_MEMORY_REQUIREMENTS = 10,
    TURNIP_OP_VK_GET_IMAGE_MEMORY_REQUIREMENTS = 11,
    TURNIP_OP_VK_BIND_BUFFER_MEMORY = 12,
    TURNIP_OP_VK_BIND_IMAGE_MEMORY = 13,
    TURNIP_OP_VK_CREATE_BUFFER = 14,
    TURNIP_OP_VK_DESTROY_BUFFER = 15,
    TURNIP_OP_VK_CREATE_IMAGE = 16,
    TURNIP_OP_VK_DESTROY_IMAGE = 17,
    TURNIP_OP_VK_CREATE_COMMAND_POOL = 18,
    TURNIP_OP_VK_DESTROY_COMMAND_POOL = 19,
    TURNIP_OP_VK_ALLOCATE_COMMAND_BUFFERS = 20,
    TURNIP_OP_VK_FREE_COMMAND_BUFFERS = 21,
    TURNIP_OP_VK_BEGIN_COMMAND_BUFFER = 22,
    TURNIP_OP_VK_END_COMMAND_BUFFER = 23,
    TURNIP_OP_VK_CREATE_RENDER_PASS = 24,
    TURNIP_OP_VK_DESTROY_RENDER_PASS = 25,
    TURNIP_OP_VK_CREATE_FRAMEBUFFER = 26,
    TURNIP_OP_VK_DESTROY_FRAMEBUFFER = 27,
    TURNIP_OP_VK_CREATE_SHADER_MODULE = 28,
    TURNIP_OP_VK_DESTROY_SHADER_MODULE = 29,
    TURNIP_OP_VK_CREATE_GRAPHICS_PIPELINES = 30,
    TURNIP_OP_VK_DESTROY_PIPELINE = 31,
    TURNIP_OP_VK_CMD_BEGIN_RENDER_PASS = 32,
    TURNIP_OP_VK_CMD_END_RENDER_PASS = 33,
    TURNIP_OP_VK_CMD_CLEAR_ATTACHMENTS = 34,
    TURNIP_OP_VK_CMD_BIND_PIPELINE = 35,
    TURNIP_OP_VK_CMD_DRAW = 36,
    TURNIP_OP_VK_QUEUE_SUBMIT = 37,
    TURNIP_OP_VK_QUEUE_WAIT_IDLE = 38,
    TURNIP_OP_GET_PHYSICAL_DEVICE_NAME = 39,
    TURNIP_OP_COUNT = 40
};

/* Caps enforced by the stub (see the header comment). */
#define TURNIP_MAX_NAMES          64
#define TURNIP_MAX_QUEUE_INFOS    64
#define TURNIP_MAX_PIPELINES      8
#define TURNIP_MAX_SUBPASSES      16
#define TURNIP_MAX_ATTACHMENTS    32
#define TURNIP_MAX_CLEAR_VALUES   16
#define TURNIP_MAX_SUBMITS        8

/* Present-mask bits for a vkCreateGraphicsPipelines create info
 * (tw_req_create_graphics_pipelines.present_masks[i]). Bit set
 * means the sub-struct is present and its bytes follow in the
 * sub-blob stream, in the fixed order: stages, vertex input, input
 * assembly, tessellation, viewport, rasterization, multisample,
 * depth stencil, color blend, dynamic. */
#define TURNIP_PIPE_HAS_VERTEX_INPUT    (1u << 0)
#define TURNIP_PIPE_HAS_INPUT_ASSEMBLY  (1u << 1)
#define TURNIP_PIPE_HAS_TESSELLATION    (1u << 2)
#define TURNIP_PIPE_HAS_VIEWPORT        (1u << 3)
#define TURNIP_PIPE_HAS_RASTERIZATION   (1u << 4)
#define TURNIP_PIPE_HAS_MULTISAMPLE     (1u << 5)
#define TURNIP_PIPE_HAS_DEPTH_STENCIL   (1u << 6)
#define TURNIP_PIPE_HAS_COLOR_BLEND     (1u << 7)
#define TURNIP_PIPE_HAS_DYNAMIC         (1u << 8)
#define TURNIP_PIPE_HAS_SAMPLE_MASK     (1u << 9)

/* Subpass flags for vkCreateRenderPass (one u32 per subpass in the
 * flags blob). */
#define TURNIP_SUBPASS_HAS_RESOLVE (1u << 0)
#define TURNIP_SUBPASS_HAS_DEPTH   (1u << 1)

/* ------------------------------------------------------------------ */
/* Per-op payloads. "req" is the fixed struct at the start of the     */
/* request payload; blobs follow it in the order listed. "resp" is    */
/* the fixed struct at the start of the response payload, followed    */
/* by the arrays it documents.                                        */
/* ------------------------------------------------------------------ */

/* PING: handshake. The client sends its wire version; the helper
 * replies with its version and whether it loaded Vulkan. */
typedef struct tw_req_ping {
    uint32_t version;
} tw_req_ping;
typedef struct tw_resp_ping {
    uint32_t version;
    uint32_t has_vulkan;
} tw_resp_ping;

/* GET_PHYSICAL_DEVICE_NAME: the helper calls
 * vkGetPhysicalDeviceProperties and returns deviceName
 * (VK_MAX_PHYSICAL_DEVICE_NAME_SIZE is 256). */
typedef struct tw_req_get_physical_device_name {
    tw_handle_t physdev;
} tw_req_get_physical_device_name;
typedef struct tw_resp_get_physical_device_name {
    char name[256];
} tw_resp_get_physical_device_name;

/* CREATE_INSTANCE. Blobs: [app name][engine name][layer names]
 * [extension names]. Names are NUL-terminated; the layer and
 * extension blobs are NUL-separated lists. A zero length means the
 * name is absent (NULL). The info and app structs are carried with
 * their pointer fields zeroed; the helper rebuilds them. */
typedef struct tw_req_create_instance {
    tw_VkInstanceCreateInfo info;
    tw_VkApplicationInfo app;
    uint32_t has_app;
    uint32_t app_name_len;
    uint32_t engine_name_len;
    uint32_t layer_names_len;
    uint32_t ext_names_len;
} tw_req_create_instance;
typedef struct tw_resp_create_instance {
    tw_handle_t instance;
} tw_resp_create_instance;

/* ENUMERATE_PHYSICAL_DEVICES. want_devices is 1 when the caller
 * passed an array. Response: count, then count u64 device ids. */
typedef struct tw_req_enumerate_physical_devices {
    tw_handle_t instance;
    uint32_t want_count;
    uint32_t want_devices;
} tw_req_enumerate_physical_devices;
typedef struct tw_resp_enumerate_physical_devices {
    uint32_t count;
    uint32_t reserved; /* padding: keeps the trailing id array 8-aligned */
} tw_resp_enumerate_physical_devices;

/* GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES. Response: count,
 * then count tw_VkQueueFamilyProperties. */
typedef struct tw_req_get_physical_device_queue_family_properties {
    tw_handle_t physdev;
    uint32_t want_count;
    uint32_t want_props;
} tw_req_get_physical_device_queue_family_properties;
typedef struct tw_resp_get_physical_device_queue_family_properties {
    uint32_t count;
    uint32_t reserved; /* padding: keeps the trailing array 8-aligned */
} tw_resp_get_physical_device_queue_family_properties;

/* GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES. */
typedef struct tw_req_get_physical_device_memory_properties {
    tw_handle_t physdev;
} tw_req_get_physical_device_memory_properties;
typedef struct tw_resp_get_physical_device_memory_properties {
    tw_VkPhysicalDeviceMemoryProperties props;
} tw_resp_get_physical_device_memory_properties;

/* CREATE_DEVICE. Blobs: [features (220 bytes if has_features)]
 * [queue create infos: tw_VkDeviceQueueCreateInfo array]
 * [priorities: concatenated floats, queueCount per info in order]
 * [layer names][extension names]. The info struct's pointers are
 * zeroed on the wire; the helper rebuilds them. Each queue info's
 * pQueuePriorities is zeroed; the helper points it into the
 * priorities blob. */
typedef struct tw_req_create_device {
    tw_handle_t physdev;
    tw_VkDeviceCreateInfo info;
    uint32_t has_features;
    uint32_t queue_infos_len;
    uint32_t priorities_len;
    uint32_t layer_names_len;
    uint32_t ext_names_len;
} tw_req_create_device;
typedef struct tw_resp_create_device {
    tw_handle_t device;
} tw_resp_create_device;

/* GET_DEVICE_QUEUE. */
typedef struct tw_req_get_device_queue {
    tw_handle_t device;
    uint32_t queue_family_index;
    uint32_t queue_index;
} tw_req_get_device_queue;
typedef struct tw_resp_get_device_queue {
    tw_handle_t queue;
} tw_resp_get_device_queue;

/* DEVICE_WAIT_IDLE. */
typedef struct tw_req_device_wait_idle {
    tw_handle_t device;
} tw_req_device_wait_idle;

/* ALLOCATE_MEMORY. */
typedef struct tw_req_allocate_memory {
    tw_handle_t device;
    tw_VkMemoryAllocateInfo info;
} tw_req_allocate_memory;
typedef struct tw_resp_allocate_memory {
    tw_handle_t memory;
} tw_resp_allocate_memory;

/* FREE_MEMORY. */
typedef struct tw_req_free_memory {
    tw_handle_t device;
    tw_handle_t memory;
} tw_req_free_memory;

/* GET_BUFFER_MEMORY_REQUIREMENTS / GET_IMAGE_MEMORY_REQUIREMENTS. */
typedef struct tw_req_get_memory_requirements {
    tw_handle_t device;
    tw_handle_t object;
} tw_req_get_memory_requirements;
typedef struct tw_resp_get_memory_requirements {
    tw_VkMemoryRequirements reqs;
} tw_resp_get_memory_requirements;

/* BIND_BUFFER_MEMORY / BIND_IMAGE_MEMORY. */
typedef struct tw_req_bind_memory {
    tw_handle_t device;
    tw_handle_t object;
    tw_handle_t memory;
    uint64_t offset;
} tw_req_bind_memory;

/* CREATE_BUFFER / CREATE_IMAGE. Blob: [queue family indices]. */
typedef struct tw_req_create_buffer {
    tw_handle_t device;
    tw_VkBufferCreateInfo info;
    uint32_t indices_len;
} tw_req_create_buffer;
typedef struct tw_resp_create_buffer {
    tw_handle_t buffer;
} tw_resp_create_buffer;

typedef struct tw_req_create_image {
    tw_handle_t device;
    tw_VkImageCreateInfo info;
    uint32_t indices_len;
} tw_req_create_image;
typedef struct tw_resp_create_image {
    tw_handle_t image;
} tw_resp_create_image;

/* DESTROY_BUFFER / DESTROY_IMAGE. */
typedef struct tw_req_destroy_object {
    tw_handle_t device;
    tw_handle_t object;
} tw_req_destroy_object;

/* CREATE_COMMAND_POOL. */
typedef struct tw_req_create_command_pool {
    tw_handle_t device;
    tw_VkCommandPoolCreateInfo info;
} tw_req_create_command_pool;
typedef struct tw_resp_create_command_pool {
    tw_handle_t pool;
} tw_resp_create_command_pool;

/* DESTROY_COMMAND_POOL. */
typedef struct tw_req_destroy_command_pool {
    tw_handle_t device;
    tw_handle_t pool;
} tw_req_destroy_command_pool;

/* ALLOCATE_COMMAND_BUFFERS. Response: count, then count u64 ids. */
typedef struct tw_req_allocate_command_buffers {
    tw_handle_t device;
    tw_VkCommandBufferAllocateInfo info;
} tw_req_allocate_command_buffers;
typedef struct tw_resp_allocate_command_buffers {
    uint32_t count;
    uint32_t reserved; /* padding: keeps the trailing id array 8-aligned */
} tw_resp_allocate_command_buffers;

/* FREE_COMMAND_BUFFERS. Blob: [u64 command buffer ids]. */
typedef struct tw_req_free_command_buffers {
    tw_handle_t device;
    tw_handle_t pool;
    uint32_t count;
} tw_req_free_command_buffers;

/* BEGIN_COMMAND_BUFFER. pInheritanceInfo must be NULL. */
typedef struct tw_req_begin_command_buffer {
    tw_handle_t cmdbuf;
    tw_VkCommandBufferBeginInfo info;
} tw_req_begin_command_buffer;

/* END_COMMAND_BUFFER. */
typedef struct tw_req_end_command_buffer {
    tw_handle_t cmdbuf;
} tw_req_end_command_buffer;

/* CREATE_RENDER_PASS. Blobs: [attachments]
 * [subpass structs] [subpass flags (u32 per subpass)]
 * [subpass refs stream: per subpass, input refs, color refs,
 * resolve refs (if the flag), depth ref (if the flag), preserve
 * indices] [dependencies]. */
typedef struct tw_req_create_render_pass {
    tw_handle_t device;
    tw_VkRenderPassCreateInfo info;
    uint32_t attachments_len;
    uint32_t subpasses_len;
    uint32_t subpass_flags_len;
    uint32_t subpass_refs_len;
    uint32_t deps_len;
} tw_req_create_render_pass;
typedef struct tw_resp_create_render_pass {
    tw_handle_t render_pass;
} tw_resp_create_render_pass;

/* DESTROY_RENDER_PASS. */
typedef struct tw_req_destroy_render_pass {
    tw_handle_t device;
    tw_handle_t render_pass;
} tw_req_destroy_render_pass;

/* CREATE_FRAMEBUFFER. Blob: [image view handles (u64 array)]. */
typedef struct tw_req_create_framebuffer {
    tw_handle_t device;
    tw_VkFramebufferCreateInfo info;
    uint32_t attachments_len;
} tw_req_create_framebuffer;
typedef struct tw_resp_create_framebuffer {
    tw_handle_t framebuffer;
} tw_resp_create_framebuffer;

/* DESTROY_FRAMEBUFFER. */
typedef struct tw_req_destroy_framebuffer {
    tw_handle_t device;
    tw_handle_t framebuffer;
} tw_req_destroy_framebuffer;

/* CREATE_SHADER_MODULE. Blob: [SPIR-V code bytes]. */
typedef struct tw_req_create_shader_module {
    tw_handle_t device;
    tw_VkShaderModuleCreateInfo info;
    uint32_t code_len;
} tw_req_create_shader_module;
typedef struct tw_resp_create_shader_module {
    tw_handle_t shader_module;
} tw_resp_create_shader_module;

/* DESTROY_SHADER_MODULE. */
typedef struct tw_req_destroy_shader_module {
    tw_handle_t device;
    tw_handle_t shader_module;
} tw_req_destroy_shader_module;

/* CREATE_GRAPHICS_PIPELINES. Blobs: [create infos array]
 * [sub-blob stream]. The stream is walked per create info, in the
 * fixed order: stages, vertex input, input assembly, tessellation,
 * viewport, rasterization, multisample, depth stencil, color blend,
 * dynamic. Each present sub-struct contributes its struct bytes
 * followed by its pointed-to arrays (stage name strings follow the
 * stage structs; sample mask follows the multisample struct when
 * the flag is set). present_masks[i] says which sub-structs are
 * present for create info i. */
typedef struct tw_req_create_graphics_pipelines {
    tw_handle_t device;
    tw_handle_t pipeline_cache;
    uint32_t create_info_count;
    uint32_t infos_len;
    uint32_t present_masks[TURNIP_MAX_PIPELINES];
    uint32_t sub_len;
} tw_req_create_graphics_pipelines;
typedef struct tw_resp_create_graphics_pipelines {
    uint32_t count;
    uint32_t reserved; /* padding: keeps the trailing id array 8-aligned */
} tw_resp_create_graphics_pipelines;

/* DESTROY_PIPELINE. */
typedef struct tw_req_destroy_pipeline {
    tw_handle_t device;
    tw_handle_t pipeline;
} tw_req_destroy_pipeline;

/* CMD_BEGIN_RENDER_PASS. Blob: [clear values]. */
typedef struct tw_req_cmd_begin_render_pass {
    tw_handle_t cmdbuf;
    tw_VkRenderPassBeginInfo info;
    uint32_t clear_values_len;
} tw_req_cmd_begin_render_pass;

/* CMD_END_RENDER_PASS. */
typedef struct tw_req_cmd_end_render_pass {
    tw_handle_t cmdbuf;
} tw_req_cmd_end_render_pass;

/* CMD_CLEAR_ATTACHMENTS. Blobs: [attachments][rects]. */
typedef struct tw_req_cmd_clear_attachments {
    tw_handle_t cmdbuf;
    uint32_t attachment_count;
    uint32_t rect_count;
} tw_req_cmd_clear_attachments;

/* CMD_BIND_PIPELINE. */
typedef struct tw_req_cmd_bind_pipeline {
    tw_handle_t cmdbuf;
    uint32_t bind_point;
    tw_handle_t pipeline;
} tw_req_cmd_bind_pipeline;

/* CMD_DRAW. */
typedef struct tw_req_cmd_draw {
    tw_handle_t cmdbuf;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
} tw_req_cmd_draw;

/* QUEUE_SUBMIT. Blob: [submit stream: per submit, the
 * tw_VkSubmitInfo struct, then wait semaphores, wait stage masks,
 * command buffers, signal semaphores, in that order]. The fence
 * is 0 (VK_NULL_HANDLE) unless the caller passed one. */
typedef struct tw_req_queue_submit {
    tw_handle_t queue;
    uint32_t submit_count;
    tw_handle_t fence;
    uint32_t submits_len;
} tw_req_queue_submit;

/* QUEUE_WAIT_IDLE. */
typedef struct tw_req_queue_wait_idle {
    tw_handle_t queue;
} tw_req_queue_wait_idle;

/* Response structs with a trailing variable array must be a
 * multiple of 8 bytes, so the array that follows them is aligned
 * for u64 ids. */
_Static_assert(sizeof(tw_resp_enumerate_physical_devices) % 8 == 0,
               "tw_resp_enumerate_physical_devices must be 8-aligned");
_Static_assert(
    sizeof(tw_resp_get_physical_device_queue_family_properties) % 8 == 0,
    "tw_resp_get_physical_device_queue_family_properties must be 8-aligned");
_Static_assert(sizeof(tw_resp_allocate_command_buffers) % 8 == 0,
               "tw_resp_allocate_command_buffers must be 8-aligned");
_Static_assert(sizeof(tw_resp_create_graphics_pipelines) % 8 == 0,
               "tw_resp_create_graphics_pipelines must be 8-aligned");

#ifdef __cplusplus
}
#endif
