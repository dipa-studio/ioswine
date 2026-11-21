/*
 * turnip.c: lsk-side Turnip stubs.
 *
 * Each of the 38 vkshim entries is a local stub with the exact
 * Vulkan signature. The stub serializes its arguments into a
 * turnip_wire.h request, sends it to the GPU helper over a Unix
 * socket, and returns the helper's result. Handles are u64 ids
 * assigned by the helper; the client stores the id as the value
 * of its opaque handle pointer and never dereferences it.
 *
 * Freestanding C, no libc. Socket I/O is raw syscalls via linux.h
 * on arm64; with -DTURNIP_HOST_TEST it uses libc sockets so the
 * host test can compile this file with host gcc. The NDK build
 * must NOT define TURNIP_HOST_TEST.
 *
 * No function-pointer tables in static data: turnip_get_proc_addr()
 * matches names with an if/else chain.
 */
#include "turnip.h"
#include "turnip_wire.h"

#ifdef TURNIP_HOST_TEST
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#else
#include "linux.h"
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
#endif

/* ------------------------------------------------------------------ */
/* Socket abstraction                                                   */
/* ------------------------------------------------------------------ */

#ifndef TURNIP_HOST_TEST
#define TU_AF_UNIX 1
#define TU_SOCK_STREAM 1
#define TU_MSG_NOSIGNAL 0x4000
#define TU_LINUX_EINTR 4

typedef struct {
    unsigned short sun_family;
    char sun_path[TURNIP_MAX_PATH];
} tu_sockaddr_un_t;
#endif

/* Connect a stream socket to a filesystem Unix socket path.
 * Returns the fd, or -1. */
static int tu_sock_connect(const char *path) {
#ifdef TURNIP_HOST_TEST
    int fd;
    struct sockaddr_un addr;
    size_t len;
    if (path == NULL) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    len = strlen(path);
    if (len >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path, len + 1);
    if (connect(fd, (struct sockaddr *)&addr,
                (socklen_t)(sizeof(addr.sun_family) + len + 1)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
#else
    int fd;
    tu_sockaddr_un_t addr;
    size_t path_len;
    size_t addr_len;
    size_t i;
    if (path == NULL) return -1;
    path_len = 0;
    while (path[path_len] != '\0') path_len++;
    if (path_len >= sizeof(addr.sun_path)) return -1;
    fd = (int)sys3(SYS_socket, TU_AF_UNIX, TU_SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = TU_AF_UNIX;
    for (i = 0; i <= path_len; i++) addr.sun_path[i] = path[i];
    addr_len = (size_t)((char *)&addr.sun_path - (char *)&addr) +
               path_len + 1;
    if (sys3(SYS_connect, fd, &addr, (long)addr_len) != 0) {
        sys1(SYS_close, fd);
        return -1;
    }
    return fd;
#endif
}

/* Close a socket fd. */
static void tu_sock_close(int fd) {
    if (fd < 0) return;
#ifdef TURNIP_HOST_TEST
    close(fd);
#else
    sys1(SYS_close, fd);
#endif
}

/* Send exactly len bytes. Returns 0, -1. */
static int tu_sock_send_all(int fd, const uint8_t *buf, size_t len) {
    while (len > 0) {
#ifdef TURNIP_HOST_TEST
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
#else
        long n = sys6(SYS_sendto, fd, (long)buf, (long)len, TU_MSG_NOSIGNAL, 0, 0);
        if (n < 0) {
            if (n == -TU_LINUX_EINTR) continue;
            return -1;
        }
#endif
        if (n == 0) return -1;
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* Receive exactly len bytes. Returns 0, -1 (EOF or error). */
static int tu_sock_recv_all(int fd, uint8_t *buf, size_t len) {
    while (len > 0) {
#ifdef TURNIP_HOST_TEST
        ssize_t n = recv(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
#else
        long n = sys6(SYS_recvfrom, fd, (long)buf, (long)len, 0, 0, 0);
        if (n < 0) {
            if (n == -TU_LINUX_EINTR) continue;
            return -1;
        }
#endif
        if (n == 0) return -1; /* EOF */
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* State and small helpers                                              */
/* ------------------------------------------------------------------ */

static int g_sock = -1;
static uint32_t g_seq = 0;
static int g_last_error = 0;
static int g_connected = 0;

/* One request and one response buffer (header + max payload). */
static _Alignas(8) uint8_t g_req_buf[sizeof(tw_req_hdr) + TURNIP_MAX_PAYLOAD];
static _Alignas(8) uint8_t g_resp_buf[sizeof(tw_resp_hdr) + TURNIP_MAX_PAYLOAD];
static uint32_t g_resp_len = 0;

static uint8_t *tu_req_payload_mut(void) {
    return g_req_buf + sizeof(tw_req_hdr);
}

static const uint8_t *tu_resp_payload(void) {
    return g_resp_buf + sizeof(tw_resp_hdr);
}

static int tu_streq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static size_t tu_strlen(const char *s) {
    size_t n = 0;
    if (s == NULL) return 0;
    while (s[n] != '\0') n++;
    return n;
}

/* A handle value as a wire id, and back. */
static tw_handle_t tu_to_id(const void *h) {
    return (tw_handle_t)(uintptr_t)h;
}

static void *tu_from_id(tw_handle_t id) {
    return (void *)(uintptr_t)id;
}

/* Total bytes of a NUL-separated name list (each name plus its
 * NUL). Returns 0 and sets *total on success, -1 when the count
 * exceeds the cap or a name is NULL. */
static int tu_str_list_len(const char *const *names, uint32_t count,
                           uint32_t *total) {
    uint32_t i;
    uint32_t sum = 0;
    if (count > TURNIP_MAX_NAMES) return -1;
    for (i = 0; i < count; i++) {
        size_t n;
        if (names == NULL || names[i] == NULL) return -1;
        n = tu_strlen(names[i]);
        if (n > TURNIP_MAX_PAYLOAD) return -1;
        sum += (uint32_t)n + 1u;
        if (sum > TURNIP_MAX_PAYLOAD) return -1;
    }
    *total = sum;
    return 0;
}

/* Pack a NUL-separated name list into dst (total bytes, from
 * tu_str_list_len). Returns 0, -1 on a bad argument. */
static int tu_pack_str_list(const char *const *names, uint32_t count,
                            uint8_t *dst, uint32_t total) {
    uint32_t i;
    uint32_t off = 0;
    uint32_t j;
    if (count == 0) return 0;
    if (names == NULL || dst == NULL) return -1;
    for (i = 0; i < count; i++) {
        size_t n;
        if (names[i] == NULL) return -1;
        n = tu_strlen(names[i]);
        for (j = 0; j <= n; j++) {
            if (off >= total) return -1;
            dst[off++] = (uint8_t)names[i][j];
        }
    }
    return off == total ? 0 : -1;
}

/* pNext must be NULL (not carried). Returns 0 when NULL, -1. */
static int tu_check_pnext(const void *pnext) {
    return pnext == NULL ? 0 : -1;
}

/* pAllocator must be NULL (custom allocators cannot be marshaled).
 * Returns 0 when NULL, -1. */
static int tu_check_allocator(const void *alloc) {
    return alloc == NULL ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* RPC                                                                  */
/* ------------------------------------------------------------------ */

/* One synchronous round-trip. The request payload must already be
 * in g_req_buf after the header (the stubs write it there). Sends
 * the header + payload_len bytes and reads the response. Returns
 * the VkResult from the response header, or TURNIP_ERR_TRANSPORT
 * when the transport failed. Sets g_last_error (0 on success) and
 * g_resp_len (response payload bytes). The response payload is
 * available through tu_resp_payload(). */
static int32_t tu_rpc(uint32_t op, uint32_t payload_len,
                      uint32_t *resp_len_out) {
    tw_req_hdr *rh = (tw_req_hdr *)g_req_buf;
    tw_resp_hdr *ph;
    uint32_t seq;
    if (g_sock < 0) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TURNIP_ERR_TRANSPORT;
    }
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TURNIP_ERR_TRANSPORT;
    }
    seq = ++g_seq;
    rh->magic = TURNIP_WIRE_MAGIC;
    rh->op = op;
    rh->seq = seq;
    rh->payload_len = payload_len;
    if (tu_sock_send_all(g_sock, g_req_buf,
                         sizeof(*rh) + payload_len) != 0) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TURNIP_ERR_TRANSPORT;
    }
    if (tu_sock_recv_all(g_sock, g_resp_buf, sizeof(tw_resp_hdr)) != 0) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TURNIP_ERR_TRANSPORT;
    }
    ph = (tw_resp_hdr *)g_resp_buf;
    if (ph->magic != TURNIP_WIRE_MAGIC || ph->op != op ||
        ph->seq != seq || ph->payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TURNIP_ERR_TRANSPORT;
    }
    if (ph->payload_len > 0) {
        if (tu_sock_recv_all(g_sock, g_resp_buf + sizeof(*ph),
                             ph->payload_len) != 0) {
            g_last_error = TURNIP_ERR_TRANSPORT;
            return TURNIP_ERR_TRANSPORT;
        }
    }
    g_resp_len = ph->payload_len;
    if (resp_len_out != NULL) *resp_len_out = ph->payload_len;
    g_last_error = ph->result < 0 ? (int)ph->result : 0;
    return ph->result;
}

/* Map an RPC result to the VkResult a stub returns. A transport
 * failure becomes DEVICE_LOST. */
static tw_VkResult tu_map_result(int32_t r) {
    if (r == TURNIP_ERR_TRANSPORT) return TW_VK_ERROR_DEVICE_LOST;
    return (tw_VkResult)r;
}

/* ------------------------------------------------------------------ */
/* Stub forward declarations (defined below; turnip_get_proc_addr     */
/* needs their addresses).                                              */
/* ------------------------------------------------------------------ */

static tw_VkResult tu_stub_vkCreateInstance(
    const tw_VkInstanceCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkInstance *pInstance);
static tw_VkResult tu_stub_vkEnumeratePhysicalDevices(
    tw_VkInstance instance, uint32_t *pPhysicalDeviceCount,
    tw_VkPhysicalDevice *pPhysicalDevices);
static void tu_stub_vkGetPhysicalDeviceQueueFamilyProperties(
    tw_VkPhysicalDevice physicalDevice,
    uint32_t *pQueueFamilyPropertyCount,
    tw_VkQueueFamilyProperties *pQueueFamilyProperties);
static void tu_stub_vkGetPhysicalDeviceMemoryProperties(
    tw_VkPhysicalDevice physicalDevice,
    tw_VkPhysicalDeviceMemoryProperties *pMemoryProperties);
static tw_VkResult tu_stub_vkCreateDevice(
    tw_VkPhysicalDevice physicalDevice,
    const tw_VkDeviceCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkDevice *pDevice);
static void tu_stub_vkGetDeviceQueue(
    tw_VkDevice device, uint32_t queueFamilyIndex,
    uint32_t queueIndex, tw_VkQueue *pQueue);
static tw_VkResult tu_stub_vkDeviceWaitIdle(tw_VkDevice device);
static tw_VkResult tu_stub_vkAllocateMemory(
    tw_VkDevice device, const tw_VkMemoryAllocateInfo *pAllocateInfo,
    const void *pAllocator, tw_VkDeviceMemory *pMemory);
static void tu_stub_vkFreeMemory(
    tw_VkDevice device, tw_VkDeviceMemory memory,
    const void *pAllocator);
static void tu_stub_vkGetBufferMemoryRequirements(
    tw_VkDevice device, tw_VkBuffer buffer,
    tw_VkMemoryRequirements *pMemoryRequirements);
static void tu_stub_vkGetImageMemoryRequirements(
    tw_VkDevice device, tw_VkImage image,
    tw_VkMemoryRequirements *pMemoryRequirements);
static tw_VkResult tu_stub_vkBindBufferMemory(
    tw_VkDevice device, tw_VkBuffer buffer,
    tw_VkDeviceMemory memory, tw_VkDeviceSize memoryOffset);
static tw_VkResult tu_stub_vkBindImageMemory(
    tw_VkDevice device, tw_VkImage image,
    tw_VkDeviceMemory memory, tw_VkDeviceSize memoryOffset);
static tw_VkResult tu_stub_vkCreateBuffer(
    tw_VkDevice device, const tw_VkBufferCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkBuffer *pBuffer);
static void tu_stub_vkDestroyBuffer(
    tw_VkDevice device, tw_VkBuffer buffer, const void *pAllocator);
static tw_VkResult tu_stub_vkCreateImage(
    tw_VkDevice device, const tw_VkImageCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkImage *pImage);
static void tu_stub_vkDestroyImage(
    tw_VkDevice device, tw_VkImage image, const void *pAllocator);
static tw_VkResult tu_stub_vkCreateCommandPool(
    tw_VkDevice device, const tw_VkCommandPoolCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkCommandPool *pCommandPool);
static void tu_stub_vkDestroyCommandPool(
    tw_VkDevice device, tw_VkCommandPool commandPool,
    const void *pAllocator);
static tw_VkResult tu_stub_vkAllocateCommandBuffers(
    tw_VkDevice device,
    const tw_VkCommandBufferAllocateInfo *pAllocateInfo,
    tw_VkCommandBuffer *pCommandBuffers);
static void tu_stub_vkFreeCommandBuffers(
    tw_VkDevice device, tw_VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const tw_VkCommandBuffer *pCommandBuffers);
static tw_VkResult tu_stub_vkBeginCommandBuffer(
    tw_VkCommandBuffer commandBuffer,
    const tw_VkCommandBufferBeginInfo *pBeginInfo);
static tw_VkResult tu_stub_vkEndCommandBuffer(
    tw_VkCommandBuffer commandBuffer);
static tw_VkResult tu_stub_vkCreateRenderPass(
    tw_VkDevice device, const tw_VkRenderPassCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkRenderPass *pRenderPass);
static void tu_stub_vkDestroyRenderPass(
    tw_VkDevice device, tw_VkRenderPass renderPass,
    const void *pAllocator);
static tw_VkResult tu_stub_vkCreateFramebuffer(
    tw_VkDevice device, const tw_VkFramebufferCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkFramebuffer *pFramebuffer);
static void tu_stub_vkDestroyFramebuffer(
    tw_VkDevice device, tw_VkFramebuffer framebuffer,
    const void *pAllocator);
static tw_VkResult tu_stub_vkCreateShaderModule(
    tw_VkDevice device,
    const tw_VkShaderModuleCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkShaderModule *pShaderModule);
static void tu_stub_vkDestroyShaderModule(
    tw_VkDevice device, tw_VkShaderModule shaderModule,
    const void *pAllocator);
static tw_VkResult tu_stub_vkCreateGraphicsPipelines(
    tw_VkDevice device, tw_VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const tw_VkGraphicsPipelineCreateInfo *pCreateInfos,
    const void *pAllocator, tw_VkPipeline *pPipelines);
static void tu_stub_vkDestroyPipeline(
    tw_VkDevice device, tw_VkPipeline pipeline,
    const void *pAllocator);
static void tu_stub_vkCmdBeginRenderPass(
    tw_VkCommandBuffer commandBuffer,
    const tw_VkRenderPassBeginInfo *pRenderPassBegin,
    uint32_t contents);
static void tu_stub_vkCmdEndRenderPass(
    tw_VkCommandBuffer commandBuffer);
static void tu_stub_vkCmdClearAttachments(
    tw_VkCommandBuffer commandBuffer, uint32_t attachmentCount,
    const tw_VkClearAttachment *pAttachments, uint32_t rectCount,
    const tw_VkClearRect *pRects);
static void tu_stub_vkCmdBindPipeline(
    tw_VkCommandBuffer commandBuffer, uint32_t pipelineBindPoint,
    tw_VkPipeline pipeline);
static void tu_stub_vkCmdDraw(
    tw_VkCommandBuffer commandBuffer, uint32_t vertexCount,
    uint32_t instanceCount, uint32_t firstVertex,
    uint32_t firstInstance);
static tw_VkResult tu_stub_vkQueueSubmit(
    tw_VkQueue queue, uint32_t submitCount,
    const tw_VkSubmitInfo *pSubmits, tw_VkFence fence);
static tw_VkResult tu_stub_vkQueueWaitIdle(tw_VkQueue queue);

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int turnip_init(const char *helper_socket_path) {
    tw_req_ping *req;
    tw_resp_ping *resp;
    uint32_t resp_len = 0;
    int32_t r;
    int fd;
    if (g_connected) turnip_shutdown();
    if (helper_socket_path == NULL) return -1;
    fd = tu_sock_connect(helper_socket_path);
    if (fd < 0) return -1;
    g_sock = fd;
    g_seq = 0;
    g_last_error = 0;
    req = (tw_req_ping *)tu_req_payload_mut();
    req->version = TURNIP_WIRE_VERSION;
    r = tu_rpc(TURNIP_OP_PING,
               (uint32_t)sizeof(*req), &resp_len);
    if (r != TW_VK_SUCCESS || resp_len < sizeof(*resp)) {
        tu_sock_close(g_sock);
        g_sock = -1;
        return -1;
    }
    resp = (tw_resp_ping *)tu_resp_payload();
    if (resp->version != TURNIP_WIRE_VERSION || resp->has_vulkan == 0) {
        tu_sock_close(g_sock);
        g_sock = -1;
        return -1;
    }
    g_connected = 1;
    return 0;
}

void turnip_shutdown(void) {
    if (g_sock >= 0) {
        tu_sock_close(g_sock);
        g_sock = -1;
    }
    g_connected = 0;
    g_last_error = 0;
    g_resp_len = 0;
}

int turnip_connected(void) {
    return g_connected;
}

int turnip_last_error(void) {
    return g_last_error;
}

int turnip_physical_device_name(tw_VkPhysicalDevice physdev,
                                char *buf, size_t buf_len) {
    tw_req_get_physical_device_name *req;
    tw_resp_get_physical_device_name *resp;
    uint32_t resp_len = 0;
    int32_t r;
    size_t i;
    if (!g_connected || buf == NULL || buf_len == 0) return -1;
    req = (tw_req_get_physical_device_name *)tu_req_payload_mut();
    req->physdev = tu_to_id(physdev);
    r = tu_rpc(TURNIP_OP_GET_PHYSICAL_DEVICE_NAME,
               (uint32_t)sizeof(*req), &resp_len);
    if (r != TW_VK_SUCCESS || resp_len < sizeof(*resp)) return -1;
    resp = (tw_resp_get_physical_device_name *)tu_resp_payload();
    for (i = 0; i + 1 < buf_len && resp->name[i] != '\0'; i++) {
        buf[i] = resp->name[i];
    }
    buf[i] = '\0';
    return 0;
}

void *turnip_get_proc_addr(const char *name) {
    if (!g_connected || name == NULL) return NULL;
    /* One if/else chain on purpose: no static function-pointer
     * table (arm64e signed pointers do not link with ld64.lld). */
    if (tu_streq(name, "vkCreateInstance")) return (void *)tu_stub_vkCreateInstance;
    if (tu_streq(name, "vkEnumeratePhysicalDevices")) return (void *)tu_stub_vkEnumeratePhysicalDevices;
    if (tu_streq(name, "vkGetPhysicalDeviceQueueFamilyProperties")) return (void *)tu_stub_vkGetPhysicalDeviceQueueFamilyProperties;
    if (tu_streq(name, "vkGetPhysicalDeviceMemoryProperties")) return (void *)tu_stub_vkGetPhysicalDeviceMemoryProperties;
    if (tu_streq(name, "vkCreateDevice")) return (void *)tu_stub_vkCreateDevice;
    if (tu_streq(name, "vkGetDeviceQueue")) return (void *)tu_stub_vkGetDeviceQueue;
    if (tu_streq(name, "vkDeviceWaitIdle")) return (void *)tu_stub_vkDeviceWaitIdle;
    if (tu_streq(name, "vkAllocateMemory")) return (void *)tu_stub_vkAllocateMemory;
    if (tu_streq(name, "vkFreeMemory")) return (void *)tu_stub_vkFreeMemory;
    if (tu_streq(name, "vkGetBufferMemoryRequirements")) return (void *)tu_stub_vkGetBufferMemoryRequirements;
    if (tu_streq(name, "vkGetImageMemoryRequirements")) return (void *)tu_stub_vkGetImageMemoryRequirements;
    if (tu_streq(name, "vkBindBufferMemory")) return (void *)tu_stub_vkBindBufferMemory;
    if (tu_streq(name, "vkBindImageMemory")) return (void *)tu_stub_vkBindImageMemory;
    if (tu_streq(name, "vkCreateBuffer")) return (void *)tu_stub_vkCreateBuffer;
    if (tu_streq(name, "vkDestroyBuffer")) return (void *)tu_stub_vkDestroyBuffer;
    if (tu_streq(name, "vkCreateImage")) return (void *)tu_stub_vkCreateImage;
    if (tu_streq(name, "vkDestroyImage")) return (void *)tu_stub_vkDestroyImage;
    if (tu_streq(name, "vkCreateCommandPool")) return (void *)tu_stub_vkCreateCommandPool;
    if (tu_streq(name, "vkDestroyCommandPool")) return (void *)tu_stub_vkDestroyCommandPool;
    if (tu_streq(name, "vkAllocateCommandBuffers")) return (void *)tu_stub_vkAllocateCommandBuffers;
    if (tu_streq(name, "vkFreeCommandBuffers")) return (void *)tu_stub_vkFreeCommandBuffers;
    if (tu_streq(name, "vkBeginCommandBuffer")) return (void *)tu_stub_vkBeginCommandBuffer;
    if (tu_streq(name, "vkEndCommandBuffer")) return (void *)tu_stub_vkEndCommandBuffer;
    if (tu_streq(name, "vkCreateRenderPass")) return (void *)tu_stub_vkCreateRenderPass;
    if (tu_streq(name, "vkDestroyRenderPass")) return (void *)tu_stub_vkDestroyRenderPass;
    if (tu_streq(name, "vkCreateFramebuffer")) return (void *)tu_stub_vkCreateFramebuffer;
    if (tu_streq(name, "vkDestroyFramebuffer")) return (void *)tu_stub_vkDestroyFramebuffer;
    if (tu_streq(name, "vkCreateShaderModule")) return (void *)tu_stub_vkCreateShaderModule;
    if (tu_streq(name, "vkDestroyShaderModule")) return (void *)tu_stub_vkDestroyShaderModule;
    if (tu_streq(name, "vkCreateGraphicsPipelines")) return (void *)tu_stub_vkCreateGraphicsPipelines;
    if (tu_streq(name, "vkDestroyPipeline")) return (void *)tu_stub_vkDestroyPipeline;
    if (tu_streq(name, "vkCmdBeginRenderPass")) return (void *)tu_stub_vkCmdBeginRenderPass;
    if (tu_streq(name, "vkCmdEndRenderPass")) return (void *)tu_stub_vkCmdEndRenderPass;
    if (tu_streq(name, "vkCmdClearAttachments")) return (void *)tu_stub_vkCmdClearAttachments;
    if (tu_streq(name, "vkCmdBindPipeline")) return (void *)tu_stub_vkCmdBindPipeline;
    if (tu_streq(name, "vkCmdDraw")) return (void *)tu_stub_vkCmdDraw;
    if (tu_streq(name, "vkQueueSubmit")) return (void *)tu_stub_vkQueueSubmit;
    if (tu_streq(name, "vkQueueWaitIdle")) return (void *)tu_stub_vkQueueWaitIdle;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Stubs: instance / physical device / device / memory                 */
/* ------------------------------------------------------------------ */

static tw_VkResult tu_stub_vkCreateInstance(
    const tw_VkInstanceCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkInstance *pInstance) {
    tw_req_create_instance *req;
    tw_resp_create_instance *resp;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t app_name_len = 0;
    uint32_t engine_name_len = 0;
    uint32_t layer_len = 0;
    uint32_t ext_len = 0;
    uint32_t payload_len;
    int32_t r;
    if (pCreateInfo == NULL || pInstance == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pCreateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    req = (tw_req_create_instance *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    memcpy(&req->info, pCreateInfo, sizeof(req->info));
    req->info.pApplicationInfo = NULL;
    req->info.ppEnabledLayerNames = NULL;
    req->info.ppEnabledExtensionNames = NULL;
    if (pCreateInfo->pApplicationInfo != NULL) {
        const tw_VkApplicationInfo *app = pCreateInfo->pApplicationInfo;
        if (tu_check_pnext(app->pNext) != 0) {
            g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
            return TW_VK_ERROR_FEATURE_NOT_PRESENT;
        }
        req->has_app = 1;
        memcpy(&req->app, app, sizeof(req->app));
        req->app.pApplicationName = NULL;
        req->app.pEngineName = NULL;
        if (app->pApplicationName != NULL)
            app_name_len = (uint32_t)tu_strlen(app->pApplicationName) + 1u;
        if (app->pEngineName != NULL)
            engine_name_len = (uint32_t)tu_strlen(app->pEngineName) + 1u;
    }
    if (tu_str_list_len(pCreateInfo->ppEnabledLayerNames,
                        pCreateInfo->enabledLayerCount, &layer_len) != 0 ||
        tu_str_list_len(pCreateInfo->ppEnabledExtensionNames,
                        pCreateInfo->enabledExtensionCount, &ext_len) != 0) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    req->app_name_len = app_name_len;
    req->engine_name_len = engine_name_len;
    req->layer_names_len = layer_len;
    req->ext_names_len = ext_len;
    payload_len = (uint32_t)sizeof(*req) + app_name_len + engine_name_len +
                  layer_len + ext_len;
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    if (app_name_len > 0) {
        memcpy(cur, pCreateInfo->pApplicationInfo->pApplicationName,
               app_name_len);
        cur += app_name_len;
    }
    if (engine_name_len > 0) {
        memcpy(cur, pCreateInfo->pApplicationInfo->pEngineName,
               engine_name_len);
        cur += engine_name_len;
    }
    if (layer_len > 0) {
        if (tu_pack_str_list(pCreateInfo->ppEnabledLayerNames,
                             pCreateInfo->enabledLayerCount, cur,
                             layer_len) != 0) {
            g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
            return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        cur += layer_len;
    }
    if (ext_len > 0) {
        if (tu_pack_str_list(pCreateInfo->ppEnabledExtensionNames,
                             pCreateInfo->enabledExtensionCount, cur,
                             ext_len) != 0) {
            g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
            return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        cur += ext_len;
    }
    r = tu_rpc(TURNIP_OP_VK_CREATE_INSTANCE,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_instance *)tu_resp_payload();
    *pInstance = (tw_VkInstance)tu_from_id(resp->instance);
    return tu_map_result(r);
}

static tw_VkResult tu_stub_vkEnumeratePhysicalDevices(
    tw_VkInstance instance, uint32_t *pPhysicalDeviceCount,
    tw_VkPhysicalDevice *pPhysicalDevices) {
    tw_req_enumerate_physical_devices *req;
    tw_resp_enumerate_physical_devices *resp;
    uint32_t resp_len = 0;
    uint32_t want_count = 0;
    uint32_t want_devices = 0;
    uint32_t i;
    int32_t r;
    if (pPhysicalDeviceCount == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pPhysicalDevices != NULL) {
        want_devices = 1;
        want_count = *pPhysicalDeviceCount;
    }
    req = (tw_req_enumerate_physical_devices *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->instance = tu_to_id(instance);
    req->want_count = want_count;
    req->want_devices = want_devices;
    r = tu_rpc(TURNIP_OP_VK_ENUMERATE_PHYSICAL_DEVICES,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_enumerate_physical_devices *)tu_resp_payload();
    /* The helper only sends ids when devices were requested; a
     * count query returns just the count. The id count is
     * min(total, want_count). */
    if (want_devices &&
        resp_len < sizeof(*resp) +
                      (size_t)(resp->count < want_count ? resp->count
                                                        : want_count) *
                          sizeof(tw_handle_t)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    *pPhysicalDeviceCount = resp->count;
    if (want_devices && pPhysicalDevices != NULL) {
        const tw_handle_t *ids =
            (const tw_handle_t *)(tu_resp_payload() + sizeof(*resp));
        uint32_t n = resp->count < want_count ? resp->count : want_count;
        for (i = 0; i < n; i++) {
            pPhysicalDevices[i] =
                (tw_VkPhysicalDevice)tu_from_id(ids[i]);
        }
    }
    return tu_map_result(r);
}

static void tu_stub_vkGetPhysicalDeviceQueueFamilyProperties(
    tw_VkPhysicalDevice physicalDevice,
    uint32_t *pQueueFamilyPropertyCount,
    tw_VkQueueFamilyProperties *pQueueFamilyProperties) {
    tw_req_get_physical_device_queue_family_properties *req;
    tw_resp_get_physical_device_queue_family_properties *resp;
    uint32_t resp_len = 0;
    uint32_t want_count = 0;
    uint32_t want_props = 0;
    uint32_t i;
    int32_t r;
    if (pQueueFamilyPropertyCount == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    if (pQueueFamilyProperties != NULL) {
        want_props = 1;
        want_count = *pQueueFamilyPropertyCount;
    }
    req = (tw_req_get_physical_device_queue_family_properties *)
        tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->physdev = tu_to_id(physicalDevice);
    req->want_count = want_count;
    req->want_props = want_props;
    r = tu_rpc(TURNIP_OP_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        *pQueueFamilyPropertyCount = 0;
        return;
    }
    resp = (tw_resp_get_physical_device_queue_family_properties *)
        tu_resp_payload();
    /* The helper only sends properties when they were requested; a
     * count query returns just the count. The property count is
     * min(total, want_count). */
    if (want_props &&
        resp_len < sizeof(*resp) +
                      (size_t)(resp->count < want_count ? resp->count
                                                        : want_count) *
                          sizeof(tw_VkQueueFamilyProperties)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        *pQueueFamilyPropertyCount = 0;
        return;
    }
    *pQueueFamilyPropertyCount = resp->count;
    if (want_props && pQueueFamilyProperties != NULL) {
        const tw_VkQueueFamilyProperties *props =
            (const tw_VkQueueFamilyProperties *)(tu_resp_payload() +
                                                 sizeof(*resp));
        uint32_t n = resp->count < want_count ? resp->count : want_count;
        for (i = 0; i < n; i++) {
            pQueueFamilyProperties[i] = props[i];
        }
    }
    g_last_error = 0;
}

static void tu_stub_vkGetPhysicalDeviceMemoryProperties(
    tw_VkPhysicalDevice physicalDevice,
    tw_VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    tw_req_get_physical_device_memory_properties *req;
    tw_resp_get_physical_device_memory_properties *resp;
    uint32_t resp_len = 0;
    int32_t r;
    if (pMemoryProperties == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    req = (tw_req_get_physical_device_memory_properties *)
        tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->physdev = tu_to_id(physicalDevice);
    r = tu_rpc(TURNIP_OP_VK_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
        return;
    }
    resp = (tw_resp_get_physical_device_memory_properties *)
        tu_resp_payload();
    *pMemoryProperties = resp->props;
    g_last_error = 0;
}

static tw_VkResult tu_stub_vkCreateDevice(
    tw_VkPhysicalDevice physicalDevice,
    const tw_VkDeviceCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkDevice *pDevice) {
    tw_req_create_device *req;
    tw_resp_create_device *resp;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t layer_len = 0;
    uint32_t ext_len = 0;
    uint32_t queue_infos_len;
    uint32_t priorities_len = 0;
    uint32_t payload_len;
    uint32_t i;
    int32_t r;
    if (pCreateInfo == NULL || pDevice == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pCreateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (pCreateInfo->queueCreateInfoCount > TURNIP_MAX_QUEUE_INFOS) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
        const tw_VkDeviceQueueCreateInfo *q = &pCreateInfo->pQueueCreateInfos[i];
        if (tu_check_pnext(q->pNext) != 0) {
            g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
            return TW_VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (q->queueCount > 0 && q->pQueuePriorities == NULL) {
            g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
            return TW_VK_ERROR_INITIALIZATION_FAILED;
        }
        priorities_len += q->queueCount * (uint32_t)sizeof(float);
    }
    if (tu_str_list_len(pCreateInfo->ppEnabledLayerNames,
                        pCreateInfo->enabledLayerCount, &layer_len) != 0 ||
        tu_str_list_len(pCreateInfo->ppEnabledExtensionNames,
                        pCreateInfo->enabledExtensionCount, &ext_len) != 0) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    queue_infos_len = pCreateInfo->queueCreateInfoCount *
                      (uint32_t)sizeof(tw_VkDeviceQueueCreateInfo);
    req = (tw_req_create_device *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->physdev = tu_to_id(physicalDevice);
    memcpy(&req->info, pCreateInfo, sizeof(req->info));
    req->info.pQueueCreateInfos = NULL;
    req->info.ppEnabledLayerNames = NULL;
    req->info.ppEnabledExtensionNames = NULL;
    req->info.pEnabledFeatures = NULL;
    req->has_features = pCreateInfo->pEnabledFeatures != NULL ? 1u : 0u;
    req->queue_infos_len = queue_infos_len;
    req->priorities_len = priorities_len;
    req->layer_names_len = layer_len;
    req->ext_names_len = ext_len;
    payload_len = (uint32_t)sizeof(*req) +
                  (req->has_features ? (uint32_t)sizeof(tw_VkPhysicalDeviceFeatures) : 0u) +
                  queue_infos_len + priorities_len + layer_len + ext_len;
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    if (req->has_features) {
        memcpy(cur, pCreateInfo->pEnabledFeatures,
               sizeof(tw_VkPhysicalDeviceFeatures));
        cur += sizeof(tw_VkPhysicalDeviceFeatures);
    }
    for (i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
        tw_VkDeviceQueueCreateInfo *dst = (tw_VkDeviceQueueCreateInfo *)cur;
        memcpy(dst, &pCreateInfo->pQueueCreateInfos[i], sizeof(*dst));
        dst->pQueuePriorities = NULL;
        cur += sizeof(*dst);
    }
    for (i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
        const tw_VkDeviceQueueCreateInfo *q =
            &pCreateInfo->pQueueCreateInfos[i];
        uint32_t n = q->queueCount * (uint32_t)sizeof(float);
        if (n > 0) {
            memcpy(cur, q->pQueuePriorities, n);
            cur += n;
        }
    }
    if (layer_len > 0) {
        if (tu_pack_str_list(pCreateInfo->ppEnabledLayerNames,
                             pCreateInfo->enabledLayerCount, cur,
                             layer_len) != 0) {
            g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
            return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        cur += layer_len;
    }
    if (ext_len > 0) {
        if (tu_pack_str_list(pCreateInfo->ppEnabledExtensionNames,
                             pCreateInfo->enabledExtensionCount, cur,
                             ext_len) != 0) {
            g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
            return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        cur += ext_len;
    }
    r = tu_rpc(TURNIP_OP_VK_CREATE_DEVICE,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_device *)tu_resp_payload();
    *pDevice = (tw_VkDevice)tu_from_id(resp->device);
    return tu_map_result(r);
}

static void tu_stub_vkGetDeviceQueue(
    tw_VkDevice device, uint32_t queueFamilyIndex,
    uint32_t queueIndex, tw_VkQueue *pQueue) {
    tw_req_get_device_queue *req;
    tw_resp_get_device_queue *resp;
    uint32_t resp_len = 0;
    int32_t r;
    if (pQueue == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    req = (tw_req_get_device_queue *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->queue_family_index = queueFamilyIndex;
    req->queue_index = queueIndex;
    r = tu_rpc(TURNIP_OP_VK_GET_DEVICE_QUEUE,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        *pQueue = NULL;
        return;
    }
    resp = (tw_resp_get_device_queue *)tu_resp_payload();
    *pQueue = (tw_VkQueue)tu_from_id(resp->queue);
    g_last_error = 0;
}

static tw_VkResult tu_stub_vkDeviceWaitIdle(tw_VkDevice device) {
    tw_req_device_wait_idle *req;
    uint32_t resp_len = 0;
    int32_t r;
    req = (tw_req_device_wait_idle *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    r = tu_rpc(TURNIP_OP_VK_DEVICE_WAIT_IDLE,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    return tu_map_result(r);
}

static tw_VkResult tu_stub_vkAllocateMemory(
    tw_VkDevice device, const tw_VkMemoryAllocateInfo *pAllocateInfo,
    const void *pAllocator, tw_VkDeviceMemory *pMemory) {
    tw_req_allocate_memory *req;
    tw_resp_allocate_memory *resp;
    uint32_t resp_len = 0;
    int32_t r;
    if (pAllocateInfo == NULL || pMemory == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pAllocateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    req = (tw_req_allocate_memory *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    memcpy(&req->info, pAllocateInfo, sizeof(req->info));
    r = tu_rpc(TURNIP_OP_VK_ALLOCATE_MEMORY,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_allocate_memory *)tu_resp_payload();
    *pMemory = (tw_VkDeviceMemory)tu_from_id(resp->memory);
    return tu_map_result(r);
}

static void tu_stub_vkFreeMemory(
    tw_VkDevice device, tw_VkDeviceMemory memory,
    const void *pAllocator) {
    tw_req_free_memory *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    req = (tw_req_free_memory *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->memory = tu_to_id(memory);
    r = tu_rpc(TURNIP_OP_VK_FREE_MEMORY,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static void tu_stub_vkGetBufferMemoryRequirements(
    tw_VkDevice device, tw_VkBuffer buffer,
    tw_VkMemoryRequirements *pMemoryRequirements) {
    tw_req_get_memory_requirements *req;
    tw_resp_get_memory_requirements *resp;
    uint32_t resp_len = 0;
    int32_t r;
    if (pMemoryRequirements == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    req = (tw_req_get_memory_requirements *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->object = tu_to_id(buffer);
    r = tu_rpc(TURNIP_OP_VK_GET_BUFFER_MEMORY_REQUIREMENTS,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
        return;
    }
    resp = (tw_resp_get_memory_requirements *)tu_resp_payload();
    *pMemoryRequirements = resp->reqs;
    g_last_error = 0;
}

static void tu_stub_vkGetImageMemoryRequirements(
    tw_VkDevice device, tw_VkImage image,
    tw_VkMemoryRequirements *pMemoryRequirements) {
    tw_req_get_memory_requirements *req;
    tw_resp_get_memory_requirements *resp;
    uint32_t resp_len = 0;
    int32_t r;
    if (pMemoryRequirements == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    req = (tw_req_get_memory_requirements *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->object = tu_to_id(image);
    r = tu_rpc(TURNIP_OP_VK_GET_IMAGE_MEMORY_REQUIREMENTS,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
        return;
    }
    resp = (tw_resp_get_memory_requirements *)tu_resp_payload();
    *pMemoryRequirements = resp->reqs;
    g_last_error = 0;
}

static tw_VkResult tu_stub_vkBindBufferMemory(
    tw_VkDevice device, tw_VkBuffer buffer,
    tw_VkDeviceMemory memory, tw_VkDeviceSize memoryOffset) {
    tw_req_bind_memory *req;
    uint32_t resp_len = 0;
    int32_t r;
    req = (tw_req_bind_memory *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->object = tu_to_id(buffer);
    req->memory = tu_to_id(memory);
    req->offset = (uint64_t)memoryOffset;
    r = tu_rpc(TURNIP_OP_VK_BIND_BUFFER_MEMORY,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    return tu_map_result(r);
}

static tw_VkResult tu_stub_vkBindImageMemory(
    tw_VkDevice device, tw_VkImage image,
    tw_VkDeviceMemory memory, tw_VkDeviceSize memoryOffset) {
    tw_req_bind_memory *req;
    uint32_t resp_len = 0;
    int32_t r;
    req = (tw_req_bind_memory *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->object = tu_to_id(image);
    req->memory = tu_to_id(memory);
    req->offset = (uint64_t)memoryOffset;
    r = tu_rpc(TURNIP_OP_VK_BIND_IMAGE_MEMORY,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    return tu_map_result(r);
}

/* ------------------------------------------------------------------ */
/* Stubs: buffer / image / command pool / command buffers             */
/* ------------------------------------------------------------------ */

static tw_VkResult tu_stub_vkCreateBuffer(
    tw_VkDevice device, const tw_VkBufferCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkBuffer *pBuffer) {
    tw_req_create_buffer *req;
    tw_resp_create_buffer *resp;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t indices_len;
    uint32_t payload_len;
    int32_t r;
    if (pCreateInfo == NULL || pBuffer == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pCreateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    indices_len = pCreateInfo->queueFamilyIndexCount *
                  (uint32_t)sizeof(uint32_t);
    if (pCreateInfo->queueFamilyIndexCount > 0 &&
        pCreateInfo->pQueueFamilyIndices == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    req = (tw_req_create_buffer *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    memcpy(&req->info, pCreateInfo, sizeof(req->info));
    req->info.pQueueFamilyIndices = NULL;
    req->indices_len = indices_len;
    payload_len = (uint32_t)sizeof(*req) + indices_len;
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    if (indices_len > 0) {
        memcpy(cur, pCreateInfo->pQueueFamilyIndices, indices_len);
    }
    r = tu_rpc(TURNIP_OP_VK_CREATE_BUFFER,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_buffer *)tu_resp_payload();
    *pBuffer = (tw_VkBuffer)tu_from_id(resp->buffer);
    return tu_map_result(r);
}

static void tu_stub_vkDestroyBuffer(
    tw_VkDevice device, tw_VkBuffer buffer, const void *pAllocator) {
    tw_req_destroy_object *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    req = (tw_req_destroy_object *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->object = tu_to_id(buffer);
    r = tu_rpc(TURNIP_OP_VK_DESTROY_BUFFER,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static tw_VkResult tu_stub_vkCreateImage(
    tw_VkDevice device, const tw_VkImageCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkImage *pImage) {
    tw_req_create_image *req;
    tw_resp_create_image *resp;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t indices_len;
    uint32_t payload_len;
    int32_t r;
    if (pCreateInfo == NULL || pImage == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pCreateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    indices_len = pCreateInfo->queueFamilyIndexCount *
                  (uint32_t)sizeof(uint32_t);
    if (pCreateInfo->queueFamilyIndexCount > 0 &&
        pCreateInfo->pQueueFamilyIndices == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    req = (tw_req_create_image *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    memcpy(&req->info, pCreateInfo, sizeof(req->info));
    req->info.pQueueFamilyIndices = NULL;
    req->indices_len = indices_len;
    payload_len = (uint32_t)sizeof(*req) + indices_len;
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    if (indices_len > 0) {
        memcpy(cur, pCreateInfo->pQueueFamilyIndices, indices_len);
    }
    r = tu_rpc(TURNIP_OP_VK_CREATE_IMAGE,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_image *)tu_resp_payload();
    *pImage = (tw_VkImage)tu_from_id(resp->image);
    return tu_map_result(r);
}

static void tu_stub_vkDestroyImage(
    tw_VkDevice device, tw_VkImage image, const void *pAllocator) {
    tw_req_destroy_object *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    req = (tw_req_destroy_object *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->object = tu_to_id(image);
    r = tu_rpc(TURNIP_OP_VK_DESTROY_IMAGE,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static tw_VkResult tu_stub_vkCreateCommandPool(
    tw_VkDevice device, const tw_VkCommandPoolCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkCommandPool *pCommandPool) {
    tw_req_create_command_pool *req;
    tw_resp_create_command_pool *resp;
    uint32_t resp_len = 0;
    int32_t r;
    if (pCreateInfo == NULL || pCommandPool == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pCreateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    req = (tw_req_create_command_pool *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    memcpy(&req->info, pCreateInfo, sizeof(req->info));
    r = tu_rpc(TURNIP_OP_VK_CREATE_COMMAND_POOL,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_command_pool *)tu_resp_payload();
    *pCommandPool = (tw_VkCommandPool)tu_from_id(resp->pool);
    return tu_map_result(r);
}

static void tu_stub_vkDestroyCommandPool(
    tw_VkDevice device, tw_VkCommandPool commandPool,
    const void *pAllocator) {
    tw_req_destroy_command_pool *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    req = (tw_req_destroy_command_pool *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->pool = tu_to_id(commandPool);
    r = tu_rpc(TURNIP_OP_VK_DESTROY_COMMAND_POOL,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static tw_VkResult tu_stub_vkAllocateCommandBuffers(
    tw_VkDevice device,
    const tw_VkCommandBufferAllocateInfo *pAllocateInfo,
    tw_VkCommandBuffer *pCommandBuffers) {
    tw_req_allocate_command_buffers *req;
    tw_resp_allocate_command_buffers *resp;
    uint32_t resp_len = 0;
    uint32_t i;
    int32_t r;
    if (pAllocateInfo == NULL || pCommandBuffers == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_pnext(pAllocateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    req = (tw_req_allocate_command_buffers *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    memcpy(&req->info, pAllocateInfo, sizeof(req->info));
    r = tu_rpc(TURNIP_OP_VK_ALLOCATE_COMMAND_BUFFERS,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_allocate_command_buffers *)tu_resp_payload();
    if (resp_len < sizeof(*resp) +
                      (size_t)resp->count * sizeof(tw_handle_t)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    {
        const tw_handle_t *ids =
            (const tw_handle_t *)(tu_resp_payload() + sizeof(*resp));
        uint32_t n = resp->count < pAllocateInfo->commandBufferCount
                         ? resp->count
                         : pAllocateInfo->commandBufferCount;
        for (i = 0; i < n; i++) {
            pCommandBuffers[i] =
                (tw_VkCommandBuffer)tu_from_id(ids[i]);
        }
    }
    return tu_map_result(r);
}

static void tu_stub_vkFreeCommandBuffers(
    tw_VkDevice device, tw_VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const tw_VkCommandBuffer *pCommandBuffers) {
    tw_req_free_command_buffers *req;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t i;
    uint32_t payload_len;
    int32_t r;
    if (commandBufferCount > 0 && pCommandBuffers == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    req = (tw_req_free_command_buffers *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->pool = tu_to_id(commandPool);
    req->count = commandBufferCount;
    payload_len = (uint32_t)sizeof(*req) +
                  commandBufferCount * (uint32_t)sizeof(tw_handle_t);
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    for (i = 0; i < commandBufferCount; i++) {
        tw_handle_t id = tu_to_id(pCommandBuffers[i]);
        memcpy(cur, &id, sizeof(id));
        cur += sizeof(id);
    }
    r = tu_rpc(TURNIP_OP_VK_FREE_COMMAND_BUFFERS,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static tw_VkResult tu_stub_vkBeginCommandBuffer(
    tw_VkCommandBuffer commandBuffer,
    const tw_VkCommandBufferBeginInfo *pBeginInfo) {
    tw_req_begin_command_buffer *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (pBeginInfo == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_pnext(pBeginInfo->pNext) != 0 ||
        pBeginInfo->pInheritanceInfo != NULL) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    req = (tw_req_begin_command_buffer *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->cmdbuf = tu_to_id(commandBuffer);
    memcpy(&req->info, pBeginInfo, sizeof(req->info));
    req->info.pInheritanceInfo = NULL;
    r = tu_rpc(TURNIP_OP_VK_BEGIN_COMMAND_BUFFER,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    return tu_map_result(r);
}

static tw_VkResult tu_stub_vkEndCommandBuffer(
    tw_VkCommandBuffer commandBuffer) {
    tw_req_end_command_buffer *req;
    uint32_t resp_len = 0;
    int32_t r;
    req = (tw_req_end_command_buffer *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->cmdbuf = tu_to_id(commandBuffer);
    r = tu_rpc(TURNIP_OP_VK_END_COMMAND_BUFFER,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    return tu_map_result(r);
}

/* ------------------------------------------------------------------ */
/* Stubs: render pass / framebuffer / shader / pipeline / commands    */
/* ------------------------------------------------------------------ */

/* Copy n bytes into the payload, advancing the cursor. Returns the
 * new cursor, or NULL when the write would run past end (later
 * writes become no-ops, so one NULL check at the end suffices). */
static uint8_t *tu_put(uint8_t *cur, uint8_t *end, const void *src,
                       size_t n) {
    if (cur == NULL) return NULL;
    if (n == 0) return cur; /* zero-length write: src may be NULL */
    if (src == NULL) return NULL;
    if ((size_t)(end - cur) < n) return NULL;
    memcpy(cur, src, n);
    return cur + n;
}

/* Length of the bytes written since base, or 0 when the cursor is
 * NULL (an earlier tu_put overflowed). Keeps the length math
 * defined even after an overflow. */
static uint32_t tu_len(const uint8_t *cur, const uint8_t *base) {
    return cur == NULL ? 0u : (uint32_t)(cur - base);
}

/* Present-mask for one graphics pipeline create info: which
 * sub-structs are present. Also validates the stage array
 * (pSpecializationInfo must be NULL, pName must be set). Returns
 * 0 and sets *mask on success, -1 on a bad argument. */
static int tu_pipeline_present_mask(
    const tw_VkGraphicsPipelineCreateInfo *info, uint32_t *mask) {
    uint32_t m = 0;
    uint32_t i;
    if (info->stageCount > 0 && info->pStages == NULL) return -1;
    for (i = 0; i < info->stageCount; i++) {
        const tw_VkPipelineShaderStageCreateInfo *s = &info->pStages[i];
        if (s->pName == NULL) return -1;
        if (s->pSpecializationInfo != NULL) return -1;
        if (tu_check_pnext(s->pNext) != 0) return -1;
    }
    if (info->pVertexInputState != NULL) {
        if (tu_check_pnext(info->pVertexInputState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_VERTEX_INPUT;
    }
    if (info->pInputAssemblyState != NULL) {
        if (tu_check_pnext(info->pInputAssemblyState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_INPUT_ASSEMBLY;
    }
    if (info->pTessellationState != NULL) {
        if (tu_check_pnext(info->pTessellationState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_TESSELLATION;
    }
    if (info->pViewportState != NULL) {
        if (tu_check_pnext(info->pViewportState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_VIEWPORT;
    }
    if (info->pRasterizationState != NULL) {
        if (tu_check_pnext(info->pRasterizationState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_RASTERIZATION;
    }
    if (info->pMultisampleState != NULL) {
        if (tu_check_pnext(info->pMultisampleState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_MULTISAMPLE;
        if (info->pMultisampleState->pSampleMask != NULL)
            m |= TURNIP_PIPE_HAS_SAMPLE_MASK;
    }
    if (info->pDepthStencilState != NULL) {
        if (tu_check_pnext(info->pDepthStencilState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_DEPTH_STENCIL;
    }
    if (info->pColorBlendState != NULL) {
        if (tu_check_pnext(info->pColorBlendState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_COLOR_BLEND;
    }
    if (info->pDynamicState != NULL) {
        if (tu_check_pnext(info->pDynamicState->pNext) != 0) return -1;
        m |= TURNIP_PIPE_HAS_DYNAMIC;
    }
    *mask = m;
    return 0;
}

/* Pack one create info's sub-blob stream (stages, then the
 * sub-structs in the fixed order of turnip_wire.h). Pointers in
 * the copied structs are zeroed; the helper rebuilds them.
 * Returns the new cursor, or NULL on overflow. */
static uint8_t *tu_pack_pipeline_sub(
    uint8_t *cur, uint8_t *end,
    const tw_VkGraphicsPipelineCreateInfo *info, uint32_t mask) {
    uint32_t i;
    /* Stages: structs, then name strings in order. */
    for (i = 0; i < info->stageCount; i++) {
        tw_VkPipelineShaderStageCreateInfo s;
        memcpy(&s, &info->pStages[i], sizeof(s));
        s.pName = NULL;
        s.pSpecializationInfo = NULL;
        cur = tu_put(cur, end, &s, sizeof(s));
    }
    for (i = 0; i < info->stageCount; i++) {
        size_t n = tu_strlen(info->pStages[i].pName) + 1;
        cur = tu_put(cur, end, info->pStages[i].pName, n);
    }
    if (mask & TURNIP_PIPE_HAS_VERTEX_INPUT) {
        const tw_VkPipelineVertexInputStateCreateInfo *v =
            info->pVertexInputState;
        tw_VkPipelineVertexInputStateCreateInfo w;
        memcpy(&w, v, sizeof(w));
        w.pVertexBindingDescriptions = NULL;
        w.pVertexAttributeDescriptions = NULL;
        cur = tu_put(cur, end, &w, sizeof(w));
        cur = tu_put(cur, end, v->pVertexBindingDescriptions,
                     (size_t)v->vertexBindingDescriptionCount *
                         sizeof(tw_VkVertexInputBindingDescription));
        cur = tu_put(cur, end, v->pVertexAttributeDescriptions,
                     (size_t)v->vertexAttributeDescriptionCount *
                         sizeof(tw_VkVertexInputAttributeDescription));
    }
    if (mask & TURNIP_PIPE_HAS_INPUT_ASSEMBLY) {
        cur = tu_put(cur, end, info->pInputAssemblyState,
                     sizeof(tw_VkPipelineInputAssemblyStateCreateInfo));
    }
    if (mask & TURNIP_PIPE_HAS_TESSELLATION) {
        cur = tu_put(cur, end, info->pTessellationState,
                     sizeof(tw_VkPipelineTessellationStateCreateInfo));
    }
    if (mask & TURNIP_PIPE_HAS_VIEWPORT) {
        const tw_VkPipelineViewportStateCreateInfo *v =
            info->pViewportState;
        tw_VkPipelineViewportStateCreateInfo w;
        memcpy(&w, v, sizeof(w));
        w.pViewports = NULL;
        w.pScissors = NULL;
        cur = tu_put(cur, end, &w, sizeof(w));
        cur = tu_put(cur, end, v->pViewports,
                     (size_t)v->viewportCount * sizeof(tw_VkViewport));
        cur = tu_put(cur, end, v->pScissors,
                     (size_t)v->scissorCount * sizeof(tw_VkRect2D));
    }
    if (mask & TURNIP_PIPE_HAS_RASTERIZATION) {
        cur = tu_put(cur, end, info->pRasterizationState,
                     sizeof(tw_VkPipelineRasterizationStateCreateInfo));
    }
    if (mask & TURNIP_PIPE_HAS_MULTISAMPLE) {
        const tw_VkPipelineMultisampleStateCreateInfo *v =
            info->pMultisampleState;
        tw_VkPipelineMultisampleStateCreateInfo w;
        uint32_t mask_words;
        memcpy(&w, v, sizeof(w));
        w.pSampleMask = NULL;
        cur = tu_put(cur, end, &w, sizeof(w));
        if (mask & TURNIP_PIPE_HAS_SAMPLE_MASK) {
            mask_words = (v->rasterizationSamples + 31u) / 32u;
            cur = tu_put(cur, end, v->pSampleMask,
                         (size_t)mask_words * sizeof(uint32_t));
        }
    }
    if (mask & TURNIP_PIPE_HAS_DEPTH_STENCIL) {
        cur = tu_put(cur, end, info->pDepthStencilState,
                     sizeof(tw_VkPipelineDepthStencilStateCreateInfo));
    }
    if (mask & TURNIP_PIPE_HAS_COLOR_BLEND) {
        const tw_VkPipelineColorBlendStateCreateInfo *v =
            info->pColorBlendState;
        tw_VkPipelineColorBlendStateCreateInfo w;
        memcpy(&w, v, sizeof(w));
        w.pAttachments = NULL;
        cur = tu_put(cur, end, &w, sizeof(w));
        cur = tu_put(cur, end, v->pAttachments,
                     (size_t)v->attachmentCount *
                         sizeof(tw_VkPipelineColorBlendAttachmentState));
    }
    if (mask & TURNIP_PIPE_HAS_DYNAMIC) {
        const tw_VkPipelineDynamicStateCreateInfo *v =
            info->pDynamicState;
        tw_VkPipelineDynamicStateCreateInfo w;
        memcpy(&w, v, sizeof(w));
        w.pDynamicStates = NULL;
        cur = tu_put(cur, end, &w, sizeof(w));
        cur = tu_put(cur, end, v->pDynamicStates,
                     (size_t)v->dynamicStateCount * sizeof(uint32_t));
    }
    return cur;
}

static tw_VkResult tu_stub_vkCreateRenderPass(
    tw_VkDevice device, const tw_VkRenderPassCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkRenderPass *pRenderPass) {
    tw_req_create_render_pass *req;
    tw_resp_create_render_pass *resp;
    uint8_t *cur;
    uint8_t *end;
    uint32_t resp_len = 0;
    uint32_t i;
    int32_t r;
    if (pCreateInfo == NULL || pRenderPass == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pCreateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (pCreateInfo->attachmentCount > TURNIP_MAX_ATTACHMENTS ||
        pCreateInfo->subpassCount > TURNIP_MAX_SUBPASSES) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (pCreateInfo->attachmentCount > 0 &&
        pCreateInfo->pAttachments == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pCreateInfo->subpassCount > 0 && pCreateInfo->pSubpasses == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pCreateInfo->dependencyCount > 0 &&
        pCreateInfo->pDependencies == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    for (i = 0; i < pCreateInfo->subpassCount; i++) {
        const tw_VkSubpassDescription *s = &pCreateInfo->pSubpasses[i];
        if (s->inputAttachmentCount > 0 && s->pInputAttachments == NULL) {
            g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
            return TW_VK_ERROR_INITIALIZATION_FAILED;
        }
        if (s->colorAttachmentCount > 0 && s->pColorAttachments == NULL) {
            g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
            return TW_VK_ERROR_INITIALIZATION_FAILED;
        }
        if (s->preserveAttachmentCount > 0 &&
            s->pPreserveAttachments == NULL) {
            g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
            return TW_VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    req = (tw_req_create_render_pass *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    memcpy(&req->info, pCreateInfo, sizeof(req->info));
    req->info.pAttachments = NULL;
    req->info.pSubpasses = NULL;
    req->info.pDependencies = NULL;
    cur = tu_req_payload_mut() + sizeof(*req);
    end = tu_req_payload_mut() + TURNIP_MAX_PAYLOAD;
    {
        uint8_t *base = cur;
        cur = tu_put(cur, end, pCreateInfo->pAttachments,
                     (size_t)pCreateInfo->attachmentCount *
                         sizeof(tw_VkAttachmentDescription));
        req->attachments_len = tu_len(cur, base);
    }
    {
        uint8_t *base = cur;
        for (i = 0; i < pCreateInfo->subpassCount; i++) {
            tw_VkSubpassDescription s;
            memcpy(&s, &pCreateInfo->pSubpasses[i], sizeof(s));
            s.pInputAttachments = NULL;
            s.pColorAttachments = NULL;
            s.pResolveAttachments = NULL;
            s.pDepthStencilAttachment = NULL;
            s.pPreserveAttachments = NULL;
            cur = tu_put(cur, end, &s, sizeof(s));
        }
        req->subpasses_len = tu_len(cur, base);
    }
    {
        uint8_t *base = cur;
        for (i = 0; i < pCreateInfo->subpassCount; i++) {
            const tw_VkSubpassDescription *s =
                &pCreateInfo->pSubpasses[i];
            uint32_t flags = 0;
            if (s->pResolveAttachments != NULL)
                flags |= TURNIP_SUBPASS_HAS_RESOLVE;
            if (s->pDepthStencilAttachment != NULL)
                flags |= TURNIP_SUBPASS_HAS_DEPTH;
            cur = tu_put(cur, end, &flags, sizeof(flags));
        }
        req->subpass_flags_len = tu_len(cur, base);
    }
    {
        uint8_t *base = cur;
        for (i = 0; i < pCreateInfo->subpassCount; i++) {
            const tw_VkSubpassDescription *s =
                &pCreateInfo->pSubpasses[i];
            cur = tu_put(cur, end, s->pInputAttachments,
                         (size_t)s->inputAttachmentCount *
                             sizeof(tw_VkAttachmentReference));
            cur = tu_put(cur, end, s->pColorAttachments,
                         (size_t)s->colorAttachmentCount *
                             sizeof(tw_VkAttachmentReference));
            if (s->pResolveAttachments != NULL) {
                cur = tu_put(cur, end, s->pResolveAttachments,
                             (size_t)s->colorAttachmentCount *
                                 sizeof(tw_VkAttachmentReference));
            }
            if (s->pDepthStencilAttachment != NULL) {
                cur = tu_put(cur, end, s->pDepthStencilAttachment,
                             sizeof(tw_VkAttachmentReference));
            }
            cur = tu_put(cur, end, s->pPreserveAttachments,
                         (size_t)s->preserveAttachmentCount *
                             sizeof(uint32_t));
        }
        req->subpass_refs_len = tu_len(cur, base);
    }
    {
        uint8_t *base = cur;
        cur = tu_put(cur, end, pCreateInfo->pDependencies,
                     (size_t)pCreateInfo->dependencyCount *
                         sizeof(tw_VkSubpassDependency));
        req->deps_len = tu_len(cur, base);
    }
    if (cur == NULL) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    r = tu_rpc(TURNIP_OP_VK_CREATE_RENDER_PASS,
               (uint32_t)(cur - tu_req_payload_mut()), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_render_pass *)tu_resp_payload();
    *pRenderPass = (tw_VkRenderPass)tu_from_id(resp->render_pass);
    return tu_map_result(r);
}

static void tu_stub_vkDestroyRenderPass(
    tw_VkDevice device, tw_VkRenderPass renderPass,
    const void *pAllocator) {
    tw_req_destroy_render_pass *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    req = (tw_req_destroy_render_pass *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->render_pass = tu_to_id(renderPass);
    r = tu_rpc(TURNIP_OP_VK_DESTROY_RENDER_PASS,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static tw_VkResult tu_stub_vkCreateFramebuffer(
    tw_VkDevice device, const tw_VkFramebufferCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkFramebuffer *pFramebuffer) {
    tw_req_create_framebuffer *req;
    tw_resp_create_framebuffer *resp;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t i;
    uint32_t payload_len;
    int32_t r;
    if (pCreateInfo == NULL || pFramebuffer == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pCreateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (pCreateInfo->attachmentCount > TURNIP_MAX_ATTACHMENTS) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (pCreateInfo->attachmentCount > 0 &&
        pCreateInfo->pAttachments == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    req = (tw_req_create_framebuffer *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    memcpy(&req->info, pCreateInfo, sizeof(req->info));
    req->info.pAttachments = NULL;
    req->attachments_len = pCreateInfo->attachmentCount *
                           (uint32_t)sizeof(tw_handle_t);
    payload_len = (uint32_t)sizeof(*req) + req->attachments_len;
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    for (i = 0; i < pCreateInfo->attachmentCount; i++) {
        tw_handle_t id = tu_to_id(pCreateInfo->pAttachments[i]);
        memcpy(cur, &id, sizeof(id));
        cur += sizeof(id);
    }
    r = tu_rpc(TURNIP_OP_VK_CREATE_FRAMEBUFFER,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_framebuffer *)tu_resp_payload();
    *pFramebuffer = (tw_VkFramebuffer)tu_from_id(resp->framebuffer);
    return tu_map_result(r);
}

static void tu_stub_vkDestroyFramebuffer(
    tw_VkDevice device, tw_VkFramebuffer framebuffer,
    const void *pAllocator) {
    tw_req_destroy_framebuffer *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    req = (tw_req_destroy_framebuffer *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->framebuffer = tu_to_id(framebuffer);
    r = tu_rpc(TURNIP_OP_VK_DESTROY_FRAMEBUFFER,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static tw_VkResult tu_stub_vkCreateShaderModule(
    tw_VkDevice device,
    const tw_VkShaderModuleCreateInfo *pCreateInfo,
    const void *pAllocator, tw_VkShaderModule *pShaderModule) {
    tw_req_create_shader_module *req;
    tw_resp_create_shader_module *resp;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t payload_len;
    int32_t r;
    if (pCreateInfo == NULL || pShaderModule == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0 ||
        tu_check_pnext(pCreateInfo->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (pCreateInfo->codeSize == 0 ||
        (pCreateInfo->codeSize % sizeof(uint32_t)) != 0 ||
        pCreateInfo->pCode == NULL ||
        pCreateInfo->codeSize > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    req = (tw_req_create_shader_module *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    memcpy(&req->info, pCreateInfo, sizeof(req->info));
    req->info.pCode = NULL;
    req->code_len = (uint32_t)pCreateInfo->codeSize;
    payload_len = (uint32_t)sizeof(*req) + req->code_len;
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    memcpy(cur, pCreateInfo->pCode, req->code_len);
    r = tu_rpc(TURNIP_OP_VK_CREATE_SHADER_MODULE,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_shader_module *)tu_resp_payload();
    *pShaderModule = (tw_VkShaderModule)tu_from_id(resp->shader_module);
    return tu_map_result(r);
}

static void tu_stub_vkDestroyShaderModule(
    tw_VkDevice device, tw_VkShaderModule shaderModule,
    const void *pAllocator) {
    tw_req_destroy_shader_module *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    req = (tw_req_destroy_shader_module *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->shader_module = tu_to_id(shaderModule);
    r = tu_rpc(TURNIP_OP_VK_DESTROY_SHADER_MODULE,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static tw_VkResult tu_stub_vkCreateGraphicsPipelines(
    tw_VkDevice device, tw_VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const tw_VkGraphicsPipelineCreateInfo *pCreateInfos,
    const void *pAllocator, tw_VkPipeline *pPipelines) {
    tw_req_create_graphics_pipelines *req;
    tw_resp_create_graphics_pipelines *resp;
    uint8_t *cur;
    uint8_t *end;
    uint32_t resp_len = 0;
    uint32_t i;
    uint32_t j;
    int32_t r;
    if (pCreateInfos == NULL || pPipelines == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return TW_VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (createInfoCount == 0 || createInfoCount > TURNIP_MAX_PIPELINES) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    req = (tw_req_create_graphics_pipelines *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->pipeline_cache = tu_to_id(pipelineCache);
    req->create_info_count = createInfoCount;
    for (i = 0; i < createInfoCount; i++) {
        if (tu_check_pnext(pCreateInfos[i].pNext) != 0) {
            g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
            return TW_VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (tu_pipeline_present_mask(&pCreateInfos[i],
                                     &req->present_masks[i]) != 0) {
            g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
            return TW_VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    end = tu_req_payload_mut() + TURNIP_MAX_PAYLOAD;
    {
        uint8_t *base = cur;
        for (i = 0; i < createInfoCount; i++) {
            tw_VkGraphicsPipelineCreateInfo w;
            memcpy(&w, &pCreateInfos[i], sizeof(w));
            w.pStages = NULL;
            w.pVertexInputState = NULL;
            w.pInputAssemblyState = NULL;
            w.pTessellationState = NULL;
            w.pViewportState = NULL;
            w.pRasterizationState = NULL;
            w.pMultisampleState = NULL;
            w.pDepthStencilState = NULL;
            w.pColorBlendState = NULL;
            w.pDynamicState = NULL;
            cur = tu_put(cur, end, &w, sizeof(w));
        }
        req->infos_len = tu_len(cur, base);
    }
    {
        uint8_t *base = cur;
        for (i = 0; i < createInfoCount; i++) {
            cur = tu_pack_pipeline_sub(cur, end, &pCreateInfos[i],
                                       req->present_masks[i]);
        }
        req->sub_len = tu_len(cur, base);
    }
    if (cur == NULL) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    r = tu_rpc(TURNIP_OP_VK_CREATE_GRAPHICS_PIPELINES,
               (uint32_t)(cur - tu_req_payload_mut()), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT ||
        resp_len < sizeof(*resp)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    resp = (tw_resp_create_graphics_pipelines *)tu_resp_payload();
    if (resp_len < sizeof(*resp) +
                      (size_t)resp->count * sizeof(tw_handle_t)) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    {
        const tw_handle_t *ids =
            (const tw_handle_t *)(tu_resp_payload() + sizeof(*resp));
        uint32_t n = resp->count < createInfoCount ? resp->count
                                                   : createInfoCount;
        for (j = 0; j < n; j++) {
            pPipelines[j] = (tw_VkPipeline)tu_from_id(ids[j]);
        }
    }
    return tu_map_result(r);
}

static void tu_stub_vkDestroyPipeline(
    tw_VkDevice device, tw_VkPipeline pipeline,
    const void *pAllocator) {
    tw_req_destroy_pipeline *req;
    uint32_t resp_len = 0;
    int32_t r;
    if (tu_check_allocator(pAllocator) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    req = (tw_req_destroy_pipeline *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->device = tu_to_id(device);
    req->pipeline = tu_to_id(pipeline);
    r = tu_rpc(TURNIP_OP_VK_DESTROY_PIPELINE,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static void tu_stub_vkCmdBeginRenderPass(
    tw_VkCommandBuffer commandBuffer,
    const tw_VkRenderPassBeginInfo *pRenderPassBegin,
    uint32_t contents) {
    tw_req_cmd_begin_render_pass *req;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t payload_len;
    int32_t r;
    (void)contents;
    if (pRenderPassBegin == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    if (tu_check_pnext(pRenderPassBegin->pNext) != 0) {
        g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    if (pRenderPassBegin->clearValueCount > TURNIP_MAX_CLEAR_VALUES) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    if (pRenderPassBegin->clearValueCount > 0 &&
        pRenderPassBegin->pClearValues == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    req = (tw_req_cmd_begin_render_pass *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->cmdbuf = tu_to_id(commandBuffer);
    memcpy(&req->info, pRenderPassBegin, sizeof(req->info));
    req->info.pClearValues = NULL;
    req->clear_values_len = pRenderPassBegin->clearValueCount *
                            (uint32_t)sizeof(tw_VkClearValue);
    payload_len = (uint32_t)sizeof(*req) + req->clear_values_len;
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    if (req->clear_values_len > 0) {
        memcpy(cur, pRenderPassBegin->pClearValues,
               req->clear_values_len);
    }
    r = tu_rpc(TURNIP_OP_VK_CMD_BEGIN_RENDER_PASS,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static void tu_stub_vkCmdEndRenderPass(
    tw_VkCommandBuffer commandBuffer) {
    tw_req_cmd_end_render_pass *req;
    uint32_t resp_len = 0;
    int32_t r;
    req = (tw_req_cmd_end_render_pass *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->cmdbuf = tu_to_id(commandBuffer);
    r = tu_rpc(TURNIP_OP_VK_CMD_END_RENDER_PASS,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static void tu_stub_vkCmdClearAttachments(
    tw_VkCommandBuffer commandBuffer, uint32_t attachmentCount,
    const tw_VkClearAttachment *pAttachments, uint32_t rectCount,
    const tw_VkClearRect *pRects) {
    tw_req_cmd_clear_attachments *req;
    uint8_t *cur;
    uint32_t resp_len = 0;
    uint32_t payload_len;
    int32_t r;
    if (attachmentCount > 0 && pAttachments == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    if (rectCount > 0 && pRects == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    req = (tw_req_cmd_clear_attachments *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->cmdbuf = tu_to_id(commandBuffer);
    req->attachment_count = attachmentCount;
    req->rect_count = rectCount;
    payload_len = (uint32_t)sizeof(*req) +
                  attachmentCount * (uint32_t)sizeof(tw_VkClearAttachment) +
                  rectCount * (uint32_t)sizeof(tw_VkClearRect);
    if (payload_len > TURNIP_MAX_PAYLOAD) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    cur = tu_req_payload_mut() + sizeof(*req);
    if (attachmentCount > 0) {
        memcpy(cur, pAttachments,
               attachmentCount * sizeof(tw_VkClearAttachment));
        cur += attachmentCount * sizeof(tw_VkClearAttachment);
    }
    if (rectCount > 0) {
        memcpy(cur, pRects, rectCount * sizeof(tw_VkClearRect));
    }
    r = tu_rpc(TURNIP_OP_VK_CMD_CLEAR_ATTACHMENTS,
               payload_len, &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static void tu_stub_vkCmdBindPipeline(
    tw_VkCommandBuffer commandBuffer, uint32_t pipelineBindPoint,
    tw_VkPipeline pipeline) {
    tw_req_cmd_bind_pipeline *req;
    uint32_t resp_len = 0;
    int32_t r;
    req = (tw_req_cmd_bind_pipeline *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->cmdbuf = tu_to_id(commandBuffer);
    req->bind_point = pipelineBindPoint;
    req->pipeline = tu_to_id(pipeline);
    r = tu_rpc(TURNIP_OP_VK_CMD_BIND_PIPELINE,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static void tu_stub_vkCmdDraw(
    tw_VkCommandBuffer commandBuffer, uint32_t vertexCount,
    uint32_t instanceCount, uint32_t firstVertex,
    uint32_t firstInstance) {
    tw_req_cmd_draw *req;
    uint32_t resp_len = 0;
    int32_t r;
    req = (tw_req_cmd_draw *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->cmdbuf = tu_to_id(commandBuffer);
    req->vertex_count = vertexCount;
    req->instance_count = instanceCount;
    req->first_vertex = firstVertex;
    req->first_instance = firstInstance;
    r = tu_rpc(TURNIP_OP_VK_CMD_DRAW,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) g_last_error = TURNIP_ERR_TRANSPORT;
}

static tw_VkResult tu_stub_vkQueueSubmit(
    tw_VkQueue queue, uint32_t submitCount,
    const tw_VkSubmitInfo *pSubmits, tw_VkFence fence) {
    tw_req_queue_submit *req;
    uint8_t *cur;
    uint8_t *end;
    uint32_t resp_len = 0;
    uint32_t i;
    uint32_t j;
    int32_t r;
    if (submitCount > TURNIP_MAX_SUBMITS) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (submitCount > 0 && pSubmits == NULL) {
        g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
        return TW_VK_ERROR_INITIALIZATION_FAILED;
    }
    for (i = 0; i < submitCount; i++) {
        if (tu_check_pnext(pSubmits[i].pNext) != 0) {
            g_last_error = TW_VK_ERROR_FEATURE_NOT_PRESENT;
            return TW_VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (pSubmits[i].waitSemaphoreCount > 0 &&
            pSubmits[i].pWaitSemaphores == NULL) {
            g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
            return TW_VK_ERROR_INITIALIZATION_FAILED;
        }
        if (pSubmits[i].commandBufferCount > 0 &&
            pSubmits[i].pCommandBuffers == NULL) {
            g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
            return TW_VK_ERROR_INITIALIZATION_FAILED;
        }
        if (pSubmits[i].signalSemaphoreCount > 0 &&
            pSubmits[i].pSignalSemaphores == NULL) {
            g_last_error = TW_VK_ERROR_INITIALIZATION_FAILED;
            return TW_VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    req = (tw_req_queue_submit *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->queue = tu_to_id(queue);
    req->submit_count = submitCount;
    req->fence = tu_to_id(fence);
    cur = tu_req_payload_mut() + sizeof(*req);
    end = tu_req_payload_mut() + TURNIP_MAX_PAYLOAD;
    {
        uint8_t *base = cur;
        for (i = 0; i < submitCount; i++) {
            const tw_VkSubmitInfo *s = &pSubmits[i];
            tw_VkSubmitInfo w;
            memcpy(&w, s, sizeof(w));
            w.pWaitSemaphores = NULL;
            w.pWaitDstStageMask = NULL;
            w.pCommandBuffers = NULL;
            w.pSignalSemaphores = NULL;
            cur = tu_put(cur, end, &w, sizeof(w));
            for (j = 0; j < s->waitSemaphoreCount; j++) {
                tw_handle_t id = tu_to_id(s->pWaitSemaphores[j]);
                cur = tu_put(cur, end, &id, sizeof(id));
            }
            cur = tu_put(cur, end, s->pWaitDstStageMask,
                         (size_t)s->waitSemaphoreCount *
                             sizeof(uint32_t));
            for (j = 0; j < s->commandBufferCount; j++) {
                tw_handle_t id = tu_to_id(s->pCommandBuffers[j]);
                cur = tu_put(cur, end, &id, sizeof(id));
            }
            for (j = 0; j < s->signalSemaphoreCount; j++) {
                tw_handle_t id = tu_to_id(s->pSignalSemaphores[j]);
                cur = tu_put(cur, end, &id, sizeof(id));
            }
        }
        req->submits_len = tu_len(cur, base);
    }
    if (cur == NULL) {
        g_last_error = TW_VK_ERROR_OUT_OF_HOST_MEMORY;
        return TW_VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    r = tu_rpc(TURNIP_OP_VK_QUEUE_SUBMIT,
               (uint32_t)(cur - tu_req_payload_mut()), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    return tu_map_result(r);
}

static tw_VkResult tu_stub_vkQueueWaitIdle(tw_VkQueue queue) {
    tw_req_queue_wait_idle *req;
    uint32_t resp_len = 0;
    int32_t r;
    req = (tw_req_queue_wait_idle *)tu_req_payload_mut();
    memset(req, 0, sizeof(*req));
    req->queue = tu_to_id(queue);
    r = tu_rpc(TURNIP_OP_VK_QUEUE_WAIT_IDLE,
               (uint32_t)sizeof(*req), &resp_len);
    if (r == TURNIP_ERR_TRANSPORT) {
        g_last_error = TURNIP_ERR_TRANSPORT;
        return TW_VK_ERROR_DEVICE_LOST;
    }
    return tu_map_result(r);
}
