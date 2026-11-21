/*
 * turnip.h: lsk-side client for the Turnip GPU helper process
 *
 *
 * The bionic-free iOS process can never dlopen Turnip, so the
 * Vulkan entry points vkshim needs are marshaling stubs in turnip.c
 * that serialize each call to the helper over a Unix socket
 * (venus-like). The helper (gpu-helper.c) executes the real Vulkan
 * calls and returns the results. See turnip_wire.h for the protocol
 * and README.md for the architecture.
 *
 * Handles are u64 ids assigned by the helper. The stubs return
 * them as opaque handle pointers (the value of the pointer IS the
 * id); the client never dereferences a handle. 0 is
 * VK_NULL_HANDLE.
 *
 * Usage:
 *   if (turnip_init("/data/local/tmp/turnip.sock") != 0) fail;
 *   struct vk_shim shim;
 *   if (vkshim_load(turnip_get_proc_addr, &shim) != 0) fail;
 *   ... call through shim.vk_* ...
 *   turnip_shutdown();
 *
 * turnip_get_proc_addr() returns a stub for each of the 38 vkshim
 * entry names, and NULL for any other name. The stubs have the
 * exact Vulkan signatures (see turnip_vktypes.h). A stub that
 * cannot reach the helper returns TW_VK_ERROR_DEVICE_LOST (or
 * TW_VK_ERROR_INITIALIZATION_FAILED for a bad argument) and records
 * the failure in turnip_last_error(); void-returning stubs record
 * it the same way, since they have no return value to carry it.
 *
 * Limits (see turnip_wire.h): pNext must be NULL,
 * pSpecializationInfo must be NULL, pInheritanceInfo must be NULL,
 * pAllocator must be NULL, and the documented caps apply. A stub
 * that hits a cap returns TW_VK_ERROR_OUT_OF_HOST_MEMORY without
 * a round-trip.
 *
 * Freestanding C, no libc. The socket code uses raw syscalls via
 * linux.h on arm64; with -DTURNIP_HOST_TEST it uses libc sockets
 * so the host test can compile this file with host gcc.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "turnip_vktypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connect to the helper at the given Unix socket filesystem path
 * and run the handshake. Returns 0 when the helper is reachable
 * and reports a working Vulkan, -1 otherwise (nothing is left
 * half-open). A second call after a successful init shuts the old
 * connection down first. */
int turnip_init(const char *helper_socket_path);

/* The get_proc_addr callback for vkshim_load(). Returns the stub
 * for one of the 38 vkshim entry names, or NULL for any other
 * name. Call only after a successful turnip_init(). */
void *turnip_get_proc_addr(const char *name);

/* Close the helper connection. Safe to call when not connected. */
void turnip_shutdown(void);

/* 1 when connected to the helper, 0 otherwise. */
int turnip_connected(void);

/* The last RPC-level failure (0 when the last call succeeded).
 * Void-returning stubs record their failure here. Values are
 * negative (TW_VK_ERROR_*) or -100 for a transport failure. */
int turnip_last_error(void);

/* Device name for a physical device id, for the host test (the
 * vkshim entry set has no vkGetPhysicalDeviceProperties). Returns
 * 0 and NUL-terminates buf on success, -1 otherwise. */
int turnip_physical_device_name(tw_VkPhysicalDevice physdev,
                                char *buf, size_t buf_len);

/* Transport failure code used by turnip_last_error(). */
#define TURNIP_ERR_TRANSPORT (-100)

/* The stub function pointer types, one per vkshim entry. The stubs
 * in turnip.c have exactly these signatures. */
typedef tw_VkResult (*tw_pfn_vkCreateInstance)(
    const tw_VkInstanceCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkInstance *pInstance);
typedef tw_VkResult (*tw_pfn_vkEnumeratePhysicalDevices)(
    tw_VkInstance instance, uint32_t *pPhysicalDeviceCount,
    tw_VkPhysicalDevice *pPhysicalDevices);
typedef void (*tw_pfn_vkGetPhysicalDeviceQueueFamilyProperties)(
    tw_VkPhysicalDevice physicalDevice,
    uint32_t *pQueueFamilyPropertyCount,
    tw_VkQueueFamilyProperties *pQueueFamilyProperties);
typedef void (*tw_pfn_vkGetPhysicalDeviceMemoryProperties)(
    tw_VkPhysicalDevice physicalDevice,
    tw_VkPhysicalDeviceMemoryProperties *pMemoryProperties);
typedef tw_VkResult (*tw_pfn_vkCreateDevice)(
    tw_VkPhysicalDevice physicalDevice,
    const tw_VkDeviceCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkDevice *pDevice);
typedef void (*tw_pfn_vkGetDeviceQueue)(
    tw_VkDevice device, uint32_t queueFamilyIndex,
    uint32_t queueIndex, tw_VkQueue *pQueue);
typedef tw_VkResult (*tw_pfn_vkDeviceWaitIdle)(tw_VkDevice device);
typedef tw_VkResult (*tw_pfn_vkAllocateMemory)(
    tw_VkDevice device, const tw_VkMemoryAllocateInfo *pAllocateInfo,
    const void *pAllocator, tw_VkDeviceMemory *pMemory);
typedef void (*tw_pfn_vkFreeMemory)(
    tw_VkDevice device, tw_VkDeviceMemory memory,
    const void *pAllocator);
typedef void (*tw_pfn_vkGetBufferMemoryRequirements)(
    tw_VkDevice device, tw_VkBuffer buffer,
    tw_VkMemoryRequirements *pMemoryRequirements);
typedef void (*tw_pfn_vkGetImageMemoryRequirements)(
    tw_VkDevice device, tw_VkImage image,
    tw_VkMemoryRequirements *pMemoryRequirements);
typedef tw_VkResult (*tw_pfn_vkBindBufferMemory)(
    tw_VkDevice device, tw_VkBuffer buffer,
    tw_VkDeviceMemory memory, tw_VkDeviceSize memoryOffset);
typedef tw_VkResult (*tw_pfn_vkBindImageMemory)(
    tw_VkDevice device, tw_VkImage image,
    tw_VkDeviceMemory memory, tw_VkDeviceSize memoryOffset);
typedef tw_VkResult (*tw_pfn_vkCreateBuffer)(
    tw_VkDevice device, const tw_VkBufferCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkBuffer *pBuffer);
typedef void (*tw_pfn_vkDestroyBuffer)(
    tw_VkDevice device, tw_VkBuffer buffer, const void *pAllocator);
typedef tw_VkResult (*tw_pfn_vkCreateImage)(
    tw_VkDevice device, const tw_VkImageCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkImage *pImage);
typedef void (*tw_pfn_vkDestroyImage)(
    tw_VkDevice device, tw_VkImage image, const void *pAllocator);
typedef tw_VkResult (*tw_pfn_vkCreateCommandPool)(
    tw_VkDevice device, const tw_VkCommandPoolCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkCommandPool *pCommandPool);
typedef void (*tw_pfn_vkDestroyCommandPool)(
    tw_VkDevice device, tw_VkCommandPool commandPool,
    const void *pAllocator);
typedef tw_VkResult (*tw_pfn_vkAllocateCommandBuffers)(
    tw_VkDevice device,
    const tw_VkCommandBufferAllocateInfo *pAllocateInfo,
    tw_VkCommandBuffer *pCommandBuffers);
typedef void (*tw_pfn_vkFreeCommandBuffers)(
    tw_VkDevice device, tw_VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const tw_VkCommandBuffer *pCommandBuffers);
typedef tw_VkResult (*tw_pfn_vkBeginCommandBuffer)(
    tw_VkCommandBuffer commandBuffer,
    const tw_VkCommandBufferBeginInfo *pBeginInfo);
typedef tw_VkResult (*tw_pfn_vkEndCommandBuffer)(
    tw_VkCommandBuffer commandBuffer);
typedef tw_VkResult (*tw_pfn_vkCreateRenderPass)(
    tw_VkDevice device, const tw_VkRenderPassCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkRenderPass *pRenderPass);
typedef void (*tw_pfn_vkDestroyRenderPass)(
    tw_VkDevice device, tw_VkRenderPass renderPass,
    const void *pAllocator);
typedef tw_VkResult (*tw_pfn_vkCreateFramebuffer)(
    tw_VkDevice device, const tw_VkFramebufferCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkFramebuffer *pFramebuffer);
typedef void (*tw_pfn_vkDestroyFramebuffer)(
    tw_VkDevice device, tw_VkFramebuffer framebuffer,
    const void *pAllocator);
typedef tw_VkResult (*tw_pfn_vkCreateShaderModule)(
    tw_VkDevice device,
    const tw_VkShaderModuleCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkShaderModule *pShaderModule);
typedef void (*tw_pfn_vkDestroyShaderModule)(
    tw_VkDevice device, tw_VkShaderModule shaderModule,
    const void *pAllocator);
typedef tw_VkResult (*tw_pfn_vkCreateGraphicsPipelines)(
    tw_VkDevice device, tw_VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const tw_VkGraphicsPipelineCreateInfo *pCreateInfos,
    const void *pAllocator, tw_VkPipeline *pPipelines);
typedef void (*tw_pfn_vkDestroyPipeline)(
    tw_VkDevice device, tw_VkPipeline pipeline,
    const void *pAllocator);
typedef void (*tw_pfn_vkCmdBeginRenderPass)(
    tw_VkCommandBuffer commandBuffer,
    const tw_VkRenderPassBeginInfo *pRenderPassBegin,
    uint32_t contents);
typedef void (*tw_pfn_vkCmdEndRenderPass)(
    tw_VkCommandBuffer commandBuffer);
typedef void (*tw_pfn_vkCmdClearAttachments)(
    tw_VkCommandBuffer commandBuffer, uint32_t attachmentCount,
    const tw_VkClearAttachment *pAttachments, uint32_t rectCount,
    const tw_VkClearRect *pRects);
typedef void (*tw_pfn_vkCmdBindPipeline)(
    tw_VkCommandBuffer commandBuffer, uint32_t pipelineBindPoint,
    tw_VkPipeline pipeline);
typedef void (*tw_pfn_vkCmdDraw)(
    tw_VkCommandBuffer commandBuffer, uint32_t vertexCount,
    uint32_t instanceCount, uint32_t firstVertex,
    uint32_t firstInstance);
typedef tw_VkResult (*tw_pfn_vkQueueSubmit)(
    tw_VkQueue queue, uint32_t submitCount,
    const tw_VkSubmitInfo *pSubmits, tw_VkFence fence);
typedef tw_VkResult (*tw_pfn_vkQueueWaitIdle)(tw_VkQueue queue);

#ifdef __cplusplus
}
#endif
