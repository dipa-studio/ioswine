/*
 * gpu-helper.c: GPU helper process for ioswine.
 *
 * The bionic-free iOS process can never dlopen Turnip, so this
 * separate process loads Vulkan and executes the real calls on
 * behalf of the lsk-side stubs (turnip.c). It speaks the
 * turnip_wire.h protocol on a Unix socket: one synchronous
 * request/response at a time, one client at a time.
 *
 * Two modes:
 *   host   (--mode host, default): dlopen the system Vulkan loader
 *          (libvulkan.so.1) and resolve through vkGetInstanceProcAddr.
 *          The host test runs this mode against Mesa llvmpipe.
 *   device (--mode device --icd libvulkan_freedreno.so): dlopen the
 *          Turnip ICD directly and resolve through
 *          vk_icdGetInstanceProcAddr. No Android loader is involved.
 *
 * Handles are u64 ids. The helper keeps the real Vulkan handle for
 * each id in a table; the client only ever sees ids. 0 is
 * VK_NULL_HANDLE.
 *
 * Usage:
 *   gpu-helper --socket /tmp/turnip.sock [--mode host|device]
 *              [--icd <path>] [--verbose]
 *
 * The helper serves clients until killed (SIGTERM/SIGINT). It
 * unlinks its socket file on exit.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <vulkan/vulkan.h>

#include "turnip_wire.h"

/* PFN_vk_icdGetInstanceProcAddr is not declared by the installed
 * headers (it lives in the loader's vk_icd.h); define it here. */
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vk_icdGetInstanceProcAddr)(
    VkInstance instance, const char *pName);

typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetDeviceProcAddr)(
    VkDevice device, const char *pName);
#include "turnip_vktypes.h"

/* ------------------------------------------------------------------ */
/* Layout checks: every tw_ struct the helper memcpys must match the  */
/* real Vulkan struct exactly. A drift fails this build, not a call.   */
/* ------------------------------------------------------------------ */

#define TW_CHECK_SIZE(tw, real) \
    _Static_assert(sizeof(tw) == sizeof(real), #tw " size != " #real)
#define TW_CHECK_OFF(tw, real, field) \
    _Static_assert(offsetof(tw, field) == offsetof(real, field), \
                   #tw "." #field " offset != " #real)

TW_CHECK_SIZE(tw_VkExtent2D, VkExtent2D);
TW_CHECK_SIZE(tw_VkExtent3D, VkExtent3D);
TW_CHECK_SIZE(tw_VkOffset2D, VkOffset2D);
TW_CHECK_SIZE(tw_VkOffset3D, VkOffset3D);
TW_CHECK_SIZE(tw_VkRect2D, VkRect2D);
TW_CHECK_SIZE(tw_VkViewport, VkViewport);
TW_CHECK_SIZE(tw_VkApplicationInfo, VkApplicationInfo);
TW_CHECK_OFF(tw_VkApplicationInfo, VkApplicationInfo, pApplicationName);
TW_CHECK_OFF(tw_VkApplicationInfo, VkApplicationInfo, apiVersion);
TW_CHECK_SIZE(tw_VkInstanceCreateInfo, VkInstanceCreateInfo);
TW_CHECK_OFF(tw_VkInstanceCreateInfo, VkInstanceCreateInfo, pApplicationInfo);
TW_CHECK_OFF(tw_VkInstanceCreateInfo, VkInstanceCreateInfo, ppEnabledLayerNames);
TW_CHECK_OFF(tw_VkInstanceCreateInfo, VkInstanceCreateInfo, ppEnabledExtensionNames);
TW_CHECK_SIZE(tw_VkQueueFamilyProperties, VkQueueFamilyProperties);
TW_CHECK_SIZE(tw_VkMemoryType, VkMemoryType);
TW_CHECK_SIZE(tw_VkMemoryHeap, VkMemoryHeap);
TW_CHECK_SIZE(tw_VkPhysicalDeviceMemoryProperties, VkPhysicalDeviceMemoryProperties);
TW_CHECK_OFF(tw_VkPhysicalDeviceMemoryProperties, VkPhysicalDeviceMemoryProperties, memoryHeaps);
TW_CHECK_SIZE(tw_VkDeviceQueueCreateInfo, VkDeviceQueueCreateInfo);
TW_CHECK_OFF(tw_VkDeviceQueueCreateInfo, VkDeviceQueueCreateInfo, pQueuePriorities);
TW_CHECK_SIZE(tw_VkPhysicalDeviceFeatures, VkPhysicalDeviceFeatures);
TW_CHECK_SIZE(tw_VkDeviceCreateInfo, VkDeviceCreateInfo);
TW_CHECK_OFF(tw_VkDeviceCreateInfo, VkDeviceCreateInfo, pQueueCreateInfos);
TW_CHECK_OFF(tw_VkDeviceCreateInfo, VkDeviceCreateInfo, pEnabledFeatures);
TW_CHECK_SIZE(tw_VkMemoryAllocateInfo, VkMemoryAllocateInfo);
TW_CHECK_SIZE(tw_VkMemoryRequirements, VkMemoryRequirements);
TW_CHECK_SIZE(tw_VkBufferCreateInfo, VkBufferCreateInfo);
TW_CHECK_OFF(tw_VkBufferCreateInfo, VkBufferCreateInfo, pQueueFamilyIndices);
TW_CHECK_SIZE(tw_VkImageCreateInfo, VkImageCreateInfo);
TW_CHECK_OFF(tw_VkImageCreateInfo, VkImageCreateInfo, extent);
TW_CHECK_OFF(tw_VkImageCreateInfo, VkImageCreateInfo, pQueueFamilyIndices);
TW_CHECK_SIZE(tw_VkCommandPoolCreateInfo, VkCommandPoolCreateInfo);
TW_CHECK_SIZE(tw_VkCommandBufferAllocateInfo, VkCommandBufferAllocateInfo);
TW_CHECK_SIZE(tw_VkCommandBufferBeginInfo, VkCommandBufferBeginInfo);
TW_CHECK_SIZE(tw_VkAttachmentDescription, VkAttachmentDescription);
TW_CHECK_SIZE(tw_VkAttachmentReference, VkAttachmentReference);
TW_CHECK_SIZE(tw_VkSubpassDescription, VkSubpassDescription);
TW_CHECK_OFF(tw_VkSubpassDescription, VkSubpassDescription, pColorAttachments);
TW_CHECK_OFF(tw_VkSubpassDescription, VkSubpassDescription, pDepthStencilAttachment);
TW_CHECK_SIZE(tw_VkSubpassDependency, VkSubpassDependency);
TW_CHECK_SIZE(tw_VkRenderPassCreateInfo, VkRenderPassCreateInfo);
TW_CHECK_SIZE(tw_VkFramebufferCreateInfo, VkFramebufferCreateInfo);
TW_CHECK_OFF(tw_VkFramebufferCreateInfo, VkFramebufferCreateInfo, pAttachments);
TW_CHECK_SIZE(tw_VkShaderModuleCreateInfo, VkShaderModuleCreateInfo);
TW_CHECK_SIZE(tw_VkPipelineShaderStageCreateInfo, VkPipelineShaderStageCreateInfo);
TW_CHECK_OFF(tw_VkPipelineShaderStageCreateInfo, VkPipelineShaderStageCreateInfo, pName);
TW_CHECK_SIZE(tw_VkVertexInputBindingDescription, VkVertexInputBindingDescription);
TW_CHECK_SIZE(tw_VkVertexInputAttributeDescription, VkVertexInputAttributeDescription);
TW_CHECK_SIZE(tw_VkPipelineVertexInputStateCreateInfo, VkPipelineVertexInputStateCreateInfo);
TW_CHECK_SIZE(tw_VkPipelineInputAssemblyStateCreateInfo, VkPipelineInputAssemblyStateCreateInfo);
TW_CHECK_SIZE(tw_VkPipelineTessellationStateCreateInfo, VkPipelineTessellationStateCreateInfo);
TW_CHECK_SIZE(tw_VkPipelineViewportStateCreateInfo, VkPipelineViewportStateCreateInfo);
TW_CHECK_SIZE(tw_VkPipelineRasterizationStateCreateInfo, VkPipelineRasterizationStateCreateInfo);
TW_CHECK_SIZE(tw_VkPipelineMultisampleStateCreateInfo, VkPipelineMultisampleStateCreateInfo);
TW_CHECK_SIZE(tw_VkStencilOpState, VkStencilOpState);
TW_CHECK_SIZE(tw_VkPipelineDepthStencilStateCreateInfo, VkPipelineDepthStencilStateCreateInfo);
TW_CHECK_OFF(tw_VkPipelineDepthStencilStateCreateInfo, VkPipelineDepthStencilStateCreateInfo, front);
TW_CHECK_OFF(tw_VkPipelineDepthStencilStateCreateInfo, VkPipelineDepthStencilStateCreateInfo, back);
TW_CHECK_SIZE(tw_VkPipelineColorBlendAttachmentState, VkPipelineColorBlendAttachmentState);
TW_CHECK_SIZE(tw_VkPipelineColorBlendStateCreateInfo, VkPipelineColorBlendStateCreateInfo);
TW_CHECK_SIZE(tw_VkPipelineDynamicStateCreateInfo, VkPipelineDynamicStateCreateInfo);
TW_CHECK_SIZE(tw_VkGraphicsPipelineCreateInfo, VkGraphicsPipelineCreateInfo);
TW_CHECK_OFF(tw_VkGraphicsPipelineCreateInfo, VkGraphicsPipelineCreateInfo, pStages);
TW_CHECK_OFF(tw_VkGraphicsPipelineCreateInfo, VkGraphicsPipelineCreateInfo, layout);
TW_CHECK_OFF(tw_VkGraphicsPipelineCreateInfo, VkGraphicsPipelineCreateInfo, basePipelineHandle);
TW_CHECK_SIZE(tw_VkClearValue, VkClearValue);
TW_CHECK_SIZE(tw_VkRenderPassBeginInfo, VkRenderPassBeginInfo);
TW_CHECK_OFF(tw_VkRenderPassBeginInfo, VkRenderPassBeginInfo, renderPass);
TW_CHECK_OFF(tw_VkRenderPassBeginInfo, VkRenderPassBeginInfo, pClearValues);
TW_CHECK_SIZE(tw_VkClearAttachment, VkClearAttachment);
TW_CHECK_SIZE(tw_VkClearRect, VkClearRect);
TW_CHECK_SIZE(tw_VkSubmitInfo, VkSubmitInfo);
TW_CHECK_OFF(tw_VkSubmitInfo, VkSubmitInfo, pWaitSemaphores);
TW_CHECK_OFF(tw_VkSubmitInfo, VkSubmitInfo, pCommandBuffers);

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

static int g_verbose = 0;
static const char *g_socket_path = NULL;
static int g_listen_fd = -1;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    if (!g_verbose) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Handle table                                                         */
/* ------------------------------------------------------------------ */

enum h_kind {
    H_INSTANCE = 1,
    H_PHYSDEV,
    H_DEVICE,
    H_QUEUE,
    H_BUFFER,
    H_IMAGE,
    H_MEMORY,
    H_POOL,
    H_CMDBUF,
    H_RENDERPASS,
    H_FRAMEBUFFER,
    H_SHADER,
    H_PIPELINE,
    H_FENCE,
    H_SEMAPHORE,
    H_IMAGEVIEW,
    H_PIPELINELAYOUT
};

typedef struct {
    uint64_t id;
    void *handle;
    int kind;
    uint64_t parent; /* instance id for a device, device id for the rest */
} h_entry_t;

static h_entry_t *g_handles = NULL;
static size_t g_handles_len = 0;
static size_t g_handles_cap = 0;
static uint64_t g_next_id = 1;

static int h_grow(void) {
    size_t ncap = g_handles_cap == 0 ? 64 : g_handles_cap * 2;
    h_entry_t *n = realloc(g_handles, ncap * sizeof(*n));
    if (n == NULL) return -1;
    g_handles = n;
    g_handles_cap = ncap;
    return 0;
}

/* Register a real handle, deduping by (handle, kind). Returns the
 * id, or 0 on allocation failure. */
static uint64_t h_register(void *handle, int kind, uint64_t parent) {
    size_t i;
    if (handle == NULL) return 0;
    for (i = 0; i < g_handles_len; i++) {
        if (g_handles[i].handle == handle && g_handles[i].kind == kind)
            return g_handles[i].id;
    }
    if (g_handles_len == g_handles_cap && h_grow() != 0) return 0;
    g_handles[g_handles_len].id = g_next_id++;
    g_handles[g_handles_len].handle = handle;
    g_handles[g_handles_len].kind = kind;
    g_handles[g_handles_len].parent = parent;
    g_handles_len++;
    return g_handles[g_handles_len - 1].id;
}

/* Look an id up. Returns the real handle, or NULL. kind 0 matches
 * any kind. */
static void *h_get(uint64_t id, int kind) {
    size_t i;
    if (id == 0) return NULL;
    for (i = 0; i < g_handles_len; i++) {
        if (g_handles[i].id == id &&
            (kind == 0 || g_handles[i].kind == kind))
            return g_handles[i].handle;
    }
    return NULL;
}

/* Parent instance id for a device id, or 0. */
static uint64_t h_parent(uint64_t id) {
    size_t i;
    for (i = 0; i < g_handles_len; i++) {
        if (g_handles[i].id == id) return g_handles[i].parent;
    }
    return 0;
}

/* Remove an id from the table (after vkFree/vkDestroy). */
static void h_remove(uint64_t id) {
    size_t i;
    for (i = 0; i < g_handles_len; i++) {
        if (g_handles[i].id == id) {
            memmove(&g_handles[i], &g_handles[i + 1],
                    (g_handles_len - i - 1) * sizeof(*g_handles));
            g_handles_len--;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Vulkan loading                                                       */
/* ------------------------------------------------------------------ */

static void *g_vk_lib = NULL;
static PFN_vkGetInstanceProcAddr g_gipa = NULL;
static PFN_vk_icdGetInstanceProcAddr g_icd_gipa = NULL;
static PFN_vkGetDeviceProcAddr g_gdpa = NULL;
static int g_mode_icd = 0;

typedef void *(*get_proc_t)(void);

/* Resolve an instance-level (or global, instance==NULL) entry. */
static void *resolve_instance(VkInstance inst, const char *name) {
    if (g_mode_icd) {
        if (g_icd_gipa == NULL) return NULL;
        return (void *)g_icd_gipa(inst, name);
    }
    if (g_gipa == NULL) return NULL;
    return (void *)g_gipa(inst, name);
}

/* Resolve a device-level entry, via the cached vkGetDeviceProcAddr
 * (resolved lazily from any live instance). */
static void *resolve_device(VkDevice dev, const char *name) {
    size_t i;
    if (g_gdpa != NULL) return (void *)g_gdpa(dev, name);
    for (i = 0; i < g_handles_len; i++) {
        if (g_handles[i].kind == H_INSTANCE) {
            VkInstance inst = (VkInstance)g_handles[i].handle;
            g_gdpa = (PFN_vkGetDeviceProcAddr)resolve_instance(
                inst, "vkGetDeviceProcAddr");
            if (g_gdpa != NULL) return (void *)g_gdpa(dev, name);
        }
    }
    return NULL;
}

/* Load Vulkan. mode_icd: 0 = loader (libvulkan), 1 = ICD directly.
 * Returns 0 on success (vkCreateInstance resolvable). */
static int vk_load(int mode_icd, const char *lib_path) {
    const char *path = lib_path;
    if (path == NULL)
        path = mode_icd ? "libvulkan_freedreno.so" : "libvulkan.so.1";
    g_mode_icd = mode_icd;
    g_vk_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (g_vk_lib == NULL) {
        fprintf(stderr, "gpu-helper: dlopen %s: %s\n", path, dlerror());
        return -1;
    }
    if (mode_icd) {
        g_icd_gipa = (PFN_vk_icdGetInstanceProcAddr)dlsym(
            g_vk_lib, "vk_icdGetInstanceProcAddr");
        if (g_icd_gipa == NULL) {
            fprintf(stderr, "gpu-helper: no vk_icdGetInstanceProcAddr in %s\n",
                    path);
            return -1;
        }
    } else {
        g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(
            g_vk_lib, "vkGetInstanceProcAddr");
        if (g_gipa == NULL) {
            fprintf(stderr, "gpu-helper: no vkGetInstanceProcAddr in %s\n",
                    path);
            return -1;
        }
    }
    /* vkCreateInstance must resolve, or there is no Vulkan here. */
    if (resolve_instance(NULL, "vkCreateInstance") == NULL) {
        fprintf(stderr, "gpu-helper: vkCreateInstance does not resolve\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Socket I/O                                                           */
/* ------------------------------------------------------------------ */

static int read_all(int fd, uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read n bytes from a blob cursor, advancing it. Returns a pointer
 * into the payload, or NULL on overrun. A zero-length read returns
 * the cursor unchanged (and never NULL for a NULL source). */
static const uint8_t *rd(const uint8_t **cur, const uint8_t *end,
                         size_t n) {
    const uint8_t *p;
    if (*cur == NULL) return NULL;
    if ((size_t)(end - *cur) < n) return NULL;
    p = *cur;
    *cur += n;
    return p;
}

/* ------------------------------------------------------------------ */
/* Op handlers. Each takes the request payload and length, writes the */
/* response payload into resp (TURNIP_MAX_PAYLOAD bytes), sets        */
/* *resp_len, and returns the VkResult. An unknown handle id returns */
/* VK_ERROR_DEVICE_LOST.                                              */
/* ------------------------------------------------------------------ */

/* Parse a NUL-separated name blob into an array of pointers into
 * the blob. count is the expected number of names. Returns 0 and
 * sets *names_out (malloc'd, caller frees; NULL when count is 0).
 * The blob must be exactly consumed. */
static int parse_names(const uint8_t *blob, uint32_t blob_len,
                       uint32_t count, const char ***names_out) {
    const char **names = NULL;
    const uint8_t *p = blob;
    const uint8_t *end = blob + blob_len;
    uint32_t i;
    *names_out = NULL;
    if (count == 0) return 0;
    if (blob == NULL) return -1;
    names = malloc(count * sizeof(*names));
    if (names == NULL) return -1;
    for (i = 0; i < count; i++) {
        const uint8_t *nul;
        if (p >= end) {
            free(names);
            return -1;
        }
        nul = memchr(p, '\0', (size_t)(end - p));
        if (nul == NULL) {
            free(names);
            return -1;
        }
        names[i] = (const char *)p;
        p = nul + 1;
    }
    if (p != end) {
        free(names);
        return -1;
    }
    *names_out = names;
    return 0;
}

static int32_t op_ping(const uint8_t *pl, uint32_t len, uint8_t *resp,
                       uint32_t *resp_len) {
    tw_req_ping *req;
    tw_resp_ping *rp;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_ping *)pl;
    if (req->version != TURNIP_WIRE_VERSION)
        return VK_ERROR_INITIALIZATION_FAILED;
    rp = (tw_resp_ping *)resp;
    rp->version = TURNIP_WIRE_VERSION;
    rp->has_vulkan = 1;
    *resp_len = sizeof(*rp);
    return VK_SUCCESS;
}

static int32_t op_get_physical_device_name(const uint8_t *pl,
                                           uint32_t len, uint8_t *resp,
                                           uint32_t *resp_len) {
    tw_req_get_physical_device_name *req;
    tw_resp_get_physical_device_name *rp;
    VkPhysicalDevice pd;
    VkInstance inst;
    PFN_vkGetPhysicalDeviceProperties fn;
    VkPhysicalDeviceProperties props;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_get_physical_device_name *)pl;
    pd = h_get(req->physdev, H_PHYSDEV);
    if (pd == NULL) return VK_ERROR_DEVICE_LOST;
    inst = h_get(h_parent(req->physdev), H_INSTANCE);
    if (inst == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkGetPhysicalDeviceProperties)resolve_instance(
        inst, "vkGetPhysicalDeviceProperties");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(pd, &props);
    rp = (tw_resp_get_physical_device_name *)resp;
    memcpy(rp->name, props.deviceName, sizeof(rp->name));
    rp->name[sizeof(rp->name) - 1] = '\0';
    *resp_len = sizeof(*rp);
    return VK_SUCCESS;
}

static int32_t op_create_instance(const uint8_t *pl, uint32_t len,
                                  uint8_t *resp, uint32_t *resp_len) {
    tw_req_create_instance *req;
    const uint8_t *cur;
    const uint8_t *end;
    const char *app_name = NULL;
    const char *engine_name = NULL;
    const char **layers = NULL;
    const char **exts = NULL;
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    PFN_vkCreateInstance fn;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_instance *)pl;
    cur = pl + sizeof(*req);
    end = pl + len;
    if (req->has_app) {
        if (req->app_name_len > 0) {
            app_name = (const char *)rd(&cur, end, req->app_name_len);
            if (app_name == NULL) return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (req->engine_name_len > 0) {
            engine_name = (const char *)rd(&cur, end, req->engine_name_len);
            if (engine_name == NULL) return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (parse_names(cur, req->layer_names_len,
                    req->info.enabledLayerCount, &layers) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    cur += req->layer_names_len;
    if (parse_names(cur, req->ext_names_len,
                    req->info.enabledExtensionCount, &exts) != 0) {
        free(layers);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    cur += req->ext_names_len;
    if (cur != end) {
        free(layers);
        free(exts);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    memset(&app, 0, sizeof(app));
    memcpy(&app, &req->app, sizeof(app));
    app.pApplicationName = app_name;
    app.pEngineName = engine_name;
    memset(&ci, 0, sizeof(ci));
    memcpy(&ci, &req->info, sizeof(ci));
    ci.pApplicationInfo = req->has_app ? &app : NULL;
    ci.ppEnabledLayerNames = layers;
    ci.ppEnabledExtensionNames = exts;
    fn = (PFN_vkCreateInstance)resolve_instance(NULL, "vkCreateInstance");
    if (fn == NULL) {
        free(layers);
        free(exts);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    r = fn(&ci, NULL, &inst);
    free(layers);
    free(exts);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(inst, H_INSTANCE, 0);
    if (id == 0) {
        PFN_vkDestroyInstance dfn =
            (PFN_vkDestroyInstance)resolve_instance(inst, "vkDestroyInstance");
        if (dfn != NULL) dfn(inst, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_create_instance *rp = (tw_resp_create_instance *)resp;
        rp->instance = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
}

static int32_t op_enumerate_physical_devices(const uint8_t *pl,
                                             uint32_t len, uint8_t *resp,
                                             uint32_t *resp_len) {
    tw_req_enumerate_physical_devices *req;
    tw_resp_enumerate_physical_devices *rp;
    VkInstance inst;
    PFN_vkEnumeratePhysicalDevices fn;
    uint32_t total = 0;
    VkResult r;
    uint32_t i;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_enumerate_physical_devices *)pl;
    inst = h_get(req->instance, H_INSTANCE);
    if (inst == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkEnumeratePhysicalDevices)resolve_instance(
        inst, "vkEnumeratePhysicalDevices");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    r = fn(inst, &total, NULL);
    if (r != VK_SUCCESS) return (int32_t)r;
    rp = (tw_resp_enumerate_physical_devices *)resp;
    rp->count = total;
    *resp_len = sizeof(*rp);
    if (req->want_devices && req->want_count > 0 && total > 0) {
        VkPhysicalDevice *devs = malloc(req->want_count * sizeof(*devs));
        uint32_t n = req->want_count;
        tw_handle_t *ids;
        if (devs == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
        r = fn(inst, &n, devs);
        /* n is the total count again; the array holds
         * min(total, want_count) entries. */
        ids = (tw_handle_t *)(resp + sizeof(*rp));
        for (i = 0; i < n && i < req->want_count; i++) {
            uint64_t id = h_register(devs[i], H_PHYSDEV, req->instance);
            ids[i] = id;
        }
        *resp_len = sizeof(*rp) +
                    (size_t)(n < req->want_count ? n : req->want_count) *
                        sizeof(tw_handle_t);
        free(devs);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) return (int32_t)r;
        return (int32_t)r;
    }
    return VK_SUCCESS;
}

static int32_t op_get_queue_family_props(const uint8_t *pl, uint32_t len,
                                         uint8_t *resp, uint32_t *resp_len) {
    tw_req_get_physical_device_queue_family_properties *req;
    tw_resp_get_physical_device_queue_family_properties *rp;
    VkPhysicalDevice pd;
    VkInstance inst;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties fn;
    uint32_t total = 0;
    uint32_t i;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_get_physical_device_queue_family_properties *)pl;
    pd = h_get(req->physdev, H_PHYSDEV);
    if (pd == NULL) return VK_ERROR_DEVICE_LOST;
    inst = h_get(h_parent(req->physdev), H_INSTANCE);
    if (inst == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)resolve_instance(
        inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(pd, &total, NULL);
    rp = (tw_resp_get_physical_device_queue_family_properties *)resp;
    rp->count = total;
    *resp_len = sizeof(*rp);
    if (req->want_props && req->want_count > 0 && total > 0) {
        VkQueueFamilyProperties *props =
            malloc(req->want_count * sizeof(*props));
        tw_VkQueueFamilyProperties *out;
        uint32_t n;
        if (props == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
        n = req->want_count;
        fn(pd, &n, props);
        /* n is the total again; the array holds min(total,
         * want_count) entries. */
        out = (tw_VkQueueFamilyProperties *)(resp + sizeof(*rp));
        for (i = 0; i < total && i < req->want_count; i++) {
            memcpy(&out[i], &props[i], sizeof(out[i]));
        }
        *resp_len = sizeof(*rp) +
                    (size_t)(total < req->want_count ? total
                                                     : req->want_count) *
                        sizeof(tw_VkQueueFamilyProperties);
        free(props);
    }
    return VK_SUCCESS;
}

static int32_t op_get_memory_props(const uint8_t *pl, uint32_t len,
                                   uint8_t *resp, uint32_t *resp_len) {
    tw_req_get_physical_device_memory_properties *req;
    tw_resp_get_physical_device_memory_properties *rp;
    VkPhysicalDevice pd;
    VkInstance inst;
    PFN_vkGetPhysicalDeviceMemoryProperties fn;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_get_physical_device_memory_properties *)pl;
    pd = h_get(req->physdev, H_PHYSDEV);
    if (pd == NULL) return VK_ERROR_DEVICE_LOST;
    inst = h_get(h_parent(req->physdev), H_INSTANCE);
    if (inst == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkGetPhysicalDeviceMemoryProperties)resolve_instance(
        inst, "vkGetPhysicalDeviceMemoryProperties");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    rp = (tw_resp_get_physical_device_memory_properties *)resp;
    fn(pd, (VkPhysicalDeviceMemoryProperties *)&rp->props);
    *resp_len = sizeof(*rp);
    return VK_SUCCESS;
}

static int32_t op_create_device(const uint8_t *pl, uint32_t len,
                                uint8_t *resp, uint32_t *resp_len) {
    tw_req_create_device *req;
    const uint8_t *cur;
    const uint8_t *end;
    VkPhysicalDevice pd;
    VkInstance inst;
    PFN_vkCreateDevice fn;
    VkDeviceCreateInfo ci;
    VkPhysicalDeviceFeatures feats;
    VkDeviceQueueCreateInfo *qinfos = NULL;
    const char **layers = NULL;
    const char **exts = NULL;
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    uint32_t i;
    uint32_t prio_off = 0;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_device *)pl;
    cur = pl + sizeof(*req);
    end = pl + len;
    pd = h_get(req->physdev, H_PHYSDEV);
    if (pd == NULL) return VK_ERROR_DEVICE_LOST;
    inst = h_get(h_parent(req->physdev), H_INSTANCE);
    if (inst == NULL) return VK_ERROR_DEVICE_LOST;
    /* Features blob. */
    if (req->has_features) {
        const uint8_t *f = rd(&cur, end, sizeof(feats));
        if (f == NULL) return VK_ERROR_INITIALIZATION_FAILED;
        memcpy(&feats, f, sizeof(feats));
    }
    /* Queue create infos. */
    if (req->queue_infos_len > 0) {
        const uint8_t *q = rd(&cur, end, req->queue_infos_len);
        if (q == NULL) return VK_ERROR_INITIALIZATION_FAILED;
        if (req->info.queueCreateInfoCount == 0) return VK_ERROR_INITIALIZATION_FAILED;
        qinfos = malloc(req->info.queueCreateInfoCount * sizeof(*qinfos));
        if (qinfos == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (i = 0; i < req->info.queueCreateInfoCount; i++) {
            memcpy(&qinfos[i],
                   q + i * sizeof(tw_VkDeviceQueueCreateInfo),
                   sizeof(qinfos[i]));
        }
        /* Priorities follow, concatenated per queue info. */
        for (i = 0; i < req->info.queueCreateInfoCount; i++) {
            if (qinfos[i].queueCount > 0) {
                const uint8_t *pr = cur + prio_off;
                if (pr + (size_t)qinfos[i].queueCount * sizeof(float) >
                    cur + req->priorities_len)
                    goto fail;
                qinfos[i].pQueuePriorities = (const float *)pr;
            } else {
                qinfos[i].pQueuePriorities = NULL;
            }
            prio_off += qinfos[i].queueCount * (uint32_t)sizeof(float);
        }
        cur += req->priorities_len;
    }
    if (parse_names(cur, req->layer_names_len,
                    req->info.enabledLayerCount, &layers) != 0)
        goto fail;
    cur += req->layer_names_len;
    if (parse_names(cur, req->ext_names_len,
                    req->info.enabledExtensionCount, &exts) != 0)
        goto fail;
    cur += req->ext_names_len;
    if (cur != end) goto fail;
    memset(&ci, 0, sizeof(ci));
    memcpy(&ci, &req->info, sizeof(ci));
    ci.pQueueCreateInfos = qinfos;
    ci.ppEnabledLayerNames = layers;
    ci.ppEnabledExtensionNames = exts;
    ci.pEnabledFeatures = req->has_features ? &feats : NULL;
    fn = (PFN_vkCreateDevice)resolve_instance(inst, "vkCreateDevice");
    if (fn == NULL) goto fail;
    r = fn(pd, &ci, NULL, &dev);
    free(qinfos);
    free(layers);
    free(exts);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(dev, H_DEVICE, h_parent(req->physdev));
    if (id == 0) {
        PFN_vkDestroyDevice dfn =
            (PFN_vkDestroyDevice)resolve_device(dev, "vkDestroyDevice");
        if (dfn != NULL) dfn(dev, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_create_device *rp = (tw_resp_create_device *)resp;
        rp->device = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
fail:
    free(qinfos);
    free(layers);
    free(exts);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static int32_t op_get_device_queue(const uint8_t *pl, uint32_t len,
                                   uint8_t *resp, uint32_t *resp_len) {
    tw_req_get_device_queue *req;
    VkDevice dev;
    PFN_vkGetDeviceQueue fn;
    VkQueue q = VK_NULL_HANDLE;
    uint64_t id;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_get_device_queue *)pl;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkGetDeviceQueue)resolve_device(dev, "vkGetDeviceQueue");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, req->queue_family_index, req->queue_index, &q);
    id = h_register(q, H_QUEUE, req->device);
    if (id == 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    {
        tw_resp_get_device_queue *rp = (tw_resp_get_device_queue *)resp;
        rp->queue = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
}

static int32_t op_device_wait_idle(const uint8_t *pl, uint32_t len,
                                   uint8_t *resp, uint32_t *resp_len) {
    tw_req_device_wait_idle *req;
    VkDevice dev;
    PFN_vkDeviceWaitIdle fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_device_wait_idle *)pl;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkDeviceWaitIdle)resolve_device(dev, "vkDeviceWaitIdle");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    return (int32_t)fn(dev);
}

static int32_t op_allocate_memory(const uint8_t *pl, uint32_t len,
                                  uint8_t *resp, uint32_t *resp_len) {
    tw_req_allocate_memory *req;
    VkDevice dev;
    PFN_vkAllocateMemory fn;
    VkMemoryAllocateInfo ai;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_allocate_memory *)pl;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&ai, &req->info, sizeof(ai));
    fn = (PFN_vkAllocateMemory)resolve_device(dev, "vkAllocateMemory");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    r = fn(dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(mem, H_MEMORY, req->device);
    if (id == 0) {
        PFN_vkFreeMemory ffn =
            (PFN_vkFreeMemory)resolve_device(dev, "vkFreeMemory");
        if (ffn != NULL) ffn(dev, mem, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_allocate_memory *rp = (tw_resp_allocate_memory *)resp;
        rp->memory = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
}

static int32_t op_free_memory(const uint8_t *pl, uint32_t len,
                              uint8_t *resp, uint32_t *resp_len) {
    tw_req_free_memory *req;
    VkDevice dev;
    VkDeviceMemory mem;
    PFN_vkFreeMemory fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_free_memory *)pl;
    dev = h_get(req->device, H_DEVICE);
    mem = h_get(req->memory, H_MEMORY);
    if (dev == NULL || mem == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkFreeMemory)resolve_device(dev, "vkFreeMemory");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, mem, NULL);
    h_remove(req->memory);
    return VK_SUCCESS;
}

static int32_t op_get_buffer_memory_requirements(const uint8_t *pl,
                                                 uint32_t len,
                                                 uint8_t *resp,
                                                 uint32_t *resp_len) {
    tw_req_get_memory_requirements *req;
    tw_resp_get_memory_requirements *rp;
    VkDevice dev;
    VkBuffer buf;
    PFN_vkGetBufferMemoryRequirements fn;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_get_memory_requirements *)pl;
    dev = h_get(req->device, H_DEVICE);
    buf = h_get(req->object, H_BUFFER);
    if (dev == NULL || buf == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkGetBufferMemoryRequirements)resolve_device(
        dev, "vkGetBufferMemoryRequirements");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    rp = (tw_resp_get_memory_requirements *)resp;
    fn(dev, buf, (VkMemoryRequirements *)&rp->reqs);
    *resp_len = sizeof(*rp);
    return VK_SUCCESS;
}

static int32_t op_get_image_memory_requirements(const uint8_t *pl,
                                                uint32_t len,
                                                uint8_t *resp,
                                                uint32_t *resp_len) {
    tw_req_get_memory_requirements *req;
    tw_resp_get_memory_requirements *rp;
    VkDevice dev;
    VkImage img;
    PFN_vkGetImageMemoryRequirements fn;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_get_memory_requirements *)pl;
    dev = h_get(req->device, H_DEVICE);
    img = h_get(req->object, H_IMAGE);
    if (dev == NULL || img == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkGetImageMemoryRequirements)resolve_device(
        dev, "vkGetImageMemoryRequirements");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    rp = (tw_resp_get_memory_requirements *)resp;
    fn(dev, img, (VkMemoryRequirements *)&rp->reqs);
    *resp_len = sizeof(*rp);
    return VK_SUCCESS;
}

static int32_t op_bind_buffer_memory(const uint8_t *pl, uint32_t len,
                                     uint8_t *resp, uint32_t *resp_len) {
    tw_req_bind_memory *req;
    VkDevice dev;
    VkBuffer buf;
    VkDeviceMemory mem;
    PFN_vkBindBufferMemory fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_bind_memory *)pl;
    dev = h_get(req->device, H_DEVICE);
    buf = h_get(req->object, H_BUFFER);
    mem = h_get(req->memory, H_MEMORY);
    if (dev == NULL || buf == NULL || mem == NULL)
        return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkBindBufferMemory)resolve_device(dev, "vkBindBufferMemory");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    return (int32_t)fn(dev, buf, mem, (VkDeviceSize)req->offset);
}

static int32_t op_bind_image_memory(const uint8_t *pl, uint32_t len,
                                    uint8_t *resp, uint32_t *resp_len) {
    tw_req_bind_memory *req;
    VkDevice dev;
    VkImage img;
    VkDeviceMemory mem;
    PFN_vkBindImageMemory fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_bind_memory *)pl;
    dev = h_get(req->device, H_DEVICE);
    img = h_get(req->object, H_IMAGE);
    mem = h_get(req->memory, H_MEMORY);
    if (dev == NULL || img == NULL || mem == NULL)
        return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkBindImageMemory)resolve_device(dev, "vkBindImageMemory");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    return (int32_t)fn(dev, img, mem, (VkDeviceSize)req->offset);
}

static int32_t op_create_buffer(const uint8_t *pl, uint32_t len,
                                uint8_t *resp, uint32_t *resp_len) {
    tw_req_create_buffer *req;
    VkDevice dev;
    PFN_vkCreateBuffer fn;
    VkBufferCreateInfo ci;
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_buffer *)pl;
    if (len < sizeof(*req) + req->indices_len)
        return VK_ERROR_INITIALIZATION_FAILED;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&ci, &req->info, sizeof(ci));
    ci.pQueueFamilyIndices =
        req->indices_len > 0
            ? (const uint32_t *)(pl + sizeof(*req))
            : NULL;
    fn = (PFN_vkCreateBuffer)resolve_device(dev, "vkCreateBuffer");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    r = fn(dev, &ci, NULL, &buf);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(buf, H_BUFFER, req->device);
    if (id == 0) {
        PFN_vkDestroyBuffer dfn = (PFN_vkDestroyBuffer)resolve_device(
            dev, "vkDestroyBuffer");
        if (dfn != NULL) dfn(dev, buf, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_create_buffer *rp = (tw_resp_create_buffer *)resp;
        rp->buffer = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
}

static int32_t op_destroy_buffer(const uint8_t *pl, uint32_t len,
                                 uint8_t *resp, uint32_t *resp_len) {
    tw_req_destroy_object *req;
    VkDevice dev;
    VkBuffer buf;
    PFN_vkDestroyBuffer fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_destroy_object *)pl;
    dev = h_get(req->device, H_DEVICE);
    buf = h_get(req->object, H_BUFFER);
    if (dev == NULL || buf == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkDestroyBuffer)resolve_device(dev, "vkDestroyBuffer");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, buf, NULL);
    h_remove(req->object);
    return VK_SUCCESS;
}

static int32_t op_create_image(const uint8_t *pl, uint32_t len,
                               uint8_t *resp, uint32_t *resp_len) {
    tw_req_create_image *req;
    VkDevice dev;
    PFN_vkCreateImage fn;
    VkImageCreateInfo ci;
    VkImage img = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_image *)pl;
    if (len < sizeof(*req) + req->indices_len)
        return VK_ERROR_INITIALIZATION_FAILED;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&ci, &req->info, sizeof(ci));
    ci.pQueueFamilyIndices =
        req->indices_len > 0
            ? (const uint32_t *)(pl + sizeof(*req))
            : NULL;
    fn = (PFN_vkCreateImage)resolve_device(dev, "vkCreateImage");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    r = fn(dev, &ci, NULL, &img);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(img, H_IMAGE, req->device);
    if (id == 0) {
        PFN_vkDestroyImage dfn =
            (PFN_vkDestroyImage)resolve_device(dev, "vkDestroyImage");
        if (dfn != NULL) dfn(dev, img, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_create_image *rp = (tw_resp_create_image *)resp;
        rp->image = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
}

static int32_t op_destroy_image(const uint8_t *pl, uint32_t len,
                                uint8_t *resp, uint32_t *resp_len) {
    tw_req_destroy_object *req;
    VkDevice dev;
    VkImage img;
    PFN_vkDestroyImage fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_destroy_object *)pl;
    dev = h_get(req->device, H_DEVICE);
    img = h_get(req->object, H_IMAGE);
    if (dev == NULL || img == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkDestroyImage)resolve_device(dev, "vkDestroyImage");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, img, NULL);
    h_remove(req->object);
    return VK_SUCCESS;
}

static int32_t op_create_command_pool(const uint8_t *pl, uint32_t len,
                                      uint8_t *resp, uint32_t *resp_len) {
    tw_req_create_command_pool *req;
    VkDevice dev;
    PFN_vkCreateCommandPool fn;
    VkCommandPoolCreateInfo ci;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_command_pool *)pl;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&ci, &req->info, sizeof(ci));
    fn = (PFN_vkCreateCommandPool)resolve_device(dev, "vkCreateCommandPool");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    r = fn(dev, &ci, NULL, &pool);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(pool, H_POOL, req->device);
    if (id == 0) {
        PFN_vkDestroyCommandPool dfn =
            (PFN_vkDestroyCommandPool)resolve_device(dev, "vkDestroyCommandPool");
        if (dfn != NULL) dfn(dev, pool, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_create_command_pool *rp =
            (tw_resp_create_command_pool *)resp;
        rp->pool = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
}

static int32_t op_destroy_command_pool(const uint8_t *pl, uint32_t len,
                                       uint8_t *resp, uint32_t *resp_len) {
    tw_req_destroy_command_pool *req;
    VkDevice dev;
    VkCommandPool pool;
    PFN_vkDestroyCommandPool fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_destroy_command_pool *)pl;
    dev = h_get(req->device, H_DEVICE);
    pool = h_get(req->pool, H_POOL);
    if (dev == NULL || pool == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkDestroyCommandPool)resolve_device(dev, "vkDestroyCommandPool");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, pool, NULL);
    h_remove(req->pool);
    return VK_SUCCESS;
}

static int32_t op_allocate_command_buffers(const uint8_t *pl, uint32_t len,
                                           uint8_t *resp,
                                           uint32_t *resp_len) {
    tw_req_allocate_command_buffers *req;
    tw_resp_allocate_command_buffers *rp;
    VkDevice dev;
    VkCommandPool pool;
    PFN_vkAllocateCommandBuffers fn;
    VkCommandBufferAllocateInfo ai;
    VkCommandBuffer *bufs = NULL;
    VkResult r;
    uint32_t i;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_allocate_command_buffers *)pl;
    dev = h_get(req->device, H_DEVICE);
    pool = h_get((tw_handle_t)(uintptr_t)req->info.commandPool, H_POOL);
    if (dev == NULL || pool == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&ai, &req->info, sizeof(ai));
    ai.commandPool = pool;
    fn = (PFN_vkAllocateCommandBuffers)resolve_device(
        dev, "vkAllocateCommandBuffers");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    if (ai.commandBufferCount == 0) {
        rp = (tw_resp_allocate_command_buffers *)resp;
        rp->count = 0;
        *resp_len = sizeof(*rp);
        return VK_SUCCESS;
    }
    bufs = malloc(ai.commandBufferCount * sizeof(*bufs));
    if (bufs == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = fn(dev, &ai, bufs);
    rp = (tw_resp_allocate_command_buffers *)resp;
    rp->count = 0;
    *resp_len = sizeof(*rp);
    if (r == VK_SUCCESS) {
        tw_handle_t *ids = (tw_handle_t *)(resp + sizeof(*rp));
        for (i = 0; i < ai.commandBufferCount; i++) {
            uint64_t id = h_register(bufs[i], H_CMDBUF, req->device);
            if (id == 0) {
                free(bufs);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            ids[i] = id;
        }
        rp->count = ai.commandBufferCount;
        *resp_len = sizeof(*rp) +
                    (size_t)ai.commandBufferCount * sizeof(tw_handle_t);
    }
    free(bufs);
    return (int32_t)r;
}

static int32_t op_free_command_buffers(const uint8_t *pl, uint32_t len,
                                       uint8_t *resp, uint32_t *resp_len) {
    tw_req_free_command_buffers *req;
    VkDevice dev;
    VkCommandPool pool;
    PFN_vkFreeCommandBuffers fn;
    VkCommandBuffer *bufs = NULL;
    uint32_t i;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_free_command_buffers *)pl;
    if (len < sizeof(*req) +
                  (size_t)req->count * sizeof(tw_handle_t))
        return VK_ERROR_INITIALIZATION_FAILED;
    dev = h_get(req->device, H_DEVICE);
    pool = h_get(req->pool, H_POOL);
    if (dev == NULL || pool == NULL) return VK_ERROR_DEVICE_LOST;
    if (req->count > 0) {
        const tw_handle_t *ids =
            (const tw_handle_t *)(pl + sizeof(*req));
        bufs = malloc(req->count * sizeof(*bufs));
        if (bufs == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (i = 0; i < req->count; i++) {
            bufs[i] = h_get(ids[i], H_CMDBUF);
            if (bufs[i] == NULL) {
                free(bufs);
                return VK_ERROR_DEVICE_LOST;
            }
        }
        fn = (PFN_vkFreeCommandBuffers)resolve_device(
            dev, "vkFreeCommandBuffers");
        if (fn == NULL) {
            free(bufs);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        fn(dev, pool, req->count, bufs);
        for (i = 0; i < req->count; i++) h_remove(ids[i]);
        free(bufs);
    }
    return VK_SUCCESS;
}

/* VkDevice for a child id whose parent is a device id (command
 * buffers, queues). Returns VK_NULL_HANDLE when unknown. */
static VkDevice dev_for(uint64_t child_id) {
    uint64_t dev_id = h_parent(child_id);
    if (dev_id == 0) return VK_NULL_HANDLE;
    return h_get(dev_id, H_DEVICE);
}

static int32_t op_begin_command_buffer(const uint8_t *pl, uint32_t len,
                                       uint8_t *resp, uint32_t *resp_len) {
    tw_req_begin_command_buffer *req;
    VkCommandBuffer cb;
    PFN_vkBeginCommandBuffer fn;
    VkCommandBufferBeginInfo bi;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_begin_command_buffer *)pl;
    cb = h_get(req->cmdbuf, H_CMDBUF);
    if (cb == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&bi, &req->info, sizeof(bi));
    bi.pInheritanceInfo = NULL;
    fn = (PFN_vkBeginCommandBuffer)resolve_device(dev_for(req->cmdbuf),
                                                  "vkBeginCommandBuffer");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    return (int32_t)fn(cb, &bi);
}

static int32_t op_end_command_buffer(const uint8_t *pl, uint32_t len,
                                     uint8_t *resp, uint32_t *resp_len) {
    tw_req_end_command_buffer *req;
    VkCommandBuffer cb;
    PFN_vkEndCommandBuffer fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_end_command_buffer *)pl;
    cb = h_get(req->cmdbuf, H_CMDBUF);
    if (cb == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkEndCommandBuffer)resolve_device(dev_for(req->cmdbuf),
                                                "vkEndCommandBuffer");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    return (int32_t)fn(cb);
}

static int32_t op_create_render_pass(const uint8_t *pl, uint32_t len,
                                     uint8_t *resp, uint32_t *resp_len) {
    tw_req_create_render_pass *req;
    VkDevice dev;
    PFN_vkCreateRenderPass fn;
    VkRenderPassCreateInfo ci;
    VkSubpassDescription *subs = NULL;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    const uint8_t *cur;
    const uint8_t *end;
    uint32_t i;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_render_pass *)pl;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    cur = pl + sizeof(*req);
    end = pl + len;
    memset(&ci, 0, sizeof(ci));
    memcpy(&ci, &req->info, sizeof(ci));
    /* Attachments: point into the blob (layout-identical). */
    if (req->info.attachmentCount > 0) {
        const uint8_t *a = rd(&cur, end, req->attachments_len);
        if (a == NULL) return VK_ERROR_INITIALIZATION_FAILED;
        ci.pAttachments = (const VkAttachmentDescription *)a;
    } else {
        ci.pAttachments = NULL;
    }
    /* Subpasses: copy into owned structs, then fix up pointers
     * from the refs stream. */
    if (req->info.subpassCount > 0) {
        const uint8_t *sblob = rd(&cur, end, req->subpasses_len);
        const uint8_t *fblob = rd(&cur, end, req->subpass_flags_len);
        const uint32_t *flags;
        if (sblob == NULL || fblob == NULL)
            return VK_ERROR_INITIALIZATION_FAILED;
        flags = (const uint32_t *)fblob;
        subs = malloc(req->info.subpassCount * sizeof(*subs));
        if (subs == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (i = 0; i < req->info.subpassCount; i++) {
            const tw_VkSubpassDescription *ts =
                (const tw_VkSubpassDescription *)(sblob +
                    i * sizeof(tw_VkSubpassDescription));
            VkSubpassDescription *s = &subs[i];
            memcpy(s, ts, sizeof(*s));
            if (ts->inputAttachmentCount > 0) {
                const uint8_t *p = rd(&cur, end,
                    (size_t)ts->inputAttachmentCount *
                        sizeof(VkAttachmentReference));
                if (p == NULL) goto fail;
                s->pInputAttachments = (const VkAttachmentReference *)p;
            } else {
                s->pInputAttachments = NULL;
            }
            if (ts->colorAttachmentCount > 0) {
                const uint8_t *p = rd(&cur, end,
                    (size_t)ts->colorAttachmentCount *
                        sizeof(VkAttachmentReference));
                if (p == NULL) goto fail;
                s->pColorAttachments = (const VkAttachmentReference *)p;
            } else {
                s->pColorAttachments = NULL;
            }
            if (flags[i] & TURNIP_SUBPASS_HAS_RESOLVE) {
                const uint8_t *p = rd(&cur, end,
                    (size_t)ts->colorAttachmentCount *
                        sizeof(VkAttachmentReference));
                if (p == NULL) goto fail;
                s->pResolveAttachments = (const VkAttachmentReference *)p;
            } else {
                s->pResolveAttachments = NULL;
            }
            if (flags[i] & TURNIP_SUBPASS_HAS_DEPTH) {
                const uint8_t *p = rd(&cur, end,
                    sizeof(VkAttachmentReference));
                if (p == NULL) goto fail;
                s->pDepthStencilAttachment =
                    (const VkAttachmentReference *)p;
            } else {
                s->pDepthStencilAttachment = NULL;
            }
            if (ts->preserveAttachmentCount > 0) {
                const uint8_t *p = rd(&cur, end,
                    (size_t)ts->preserveAttachmentCount *
                        sizeof(uint32_t));
                if (p == NULL) goto fail;
                s->pPreserveAttachments = (const uint32_t *)p;
            } else {
                s->pPreserveAttachments = NULL;
            }
        }
        ci.pSubpasses = subs;
    } else {
        ci.pSubpasses = NULL;
    }
    if (req->info.dependencyCount > 0) {
        const uint8_t *d = rd(&cur, end, req->deps_len);
        if (d == NULL) goto fail;
        ci.pDependencies = (const VkSubpassDependency *)d;
    } else {
        ci.pDependencies = NULL;
    }
    if (cur != end) goto fail;
    fn = (PFN_vkCreateRenderPass)resolve_device(dev, "vkCreateRenderPass");
    if (fn == NULL) goto fail;
    r = fn(dev, &ci, NULL, &rp);
    free(subs);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(rp, H_RENDERPASS, req->device);
    if (id == 0) {
        PFN_vkDestroyRenderPass dfn =
            (PFN_vkDestroyRenderPass)resolve_device(dev, "vkDestroyRenderPass");
        if (dfn != NULL) dfn(dev, rp, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_create_render_pass *rp2 =
            (tw_resp_create_render_pass *)resp;
        rp2->render_pass = id;
        *resp_len = sizeof(*rp2);
    }
    return VK_SUCCESS;
fail:
    free(subs);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static int32_t op_destroy_render_pass(const uint8_t *pl, uint32_t len,
                                      uint8_t *resp, uint32_t *resp_len) {
    tw_req_destroy_render_pass *req;
    VkDevice dev;
    VkRenderPass rp;
    PFN_vkDestroyRenderPass fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_destroy_render_pass *)pl;
    dev = h_get(req->device, H_DEVICE);
    rp = h_get(req->render_pass, H_RENDERPASS);
    if (dev == NULL || rp == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkDestroyRenderPass)resolve_device(dev, "vkDestroyRenderPass");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, rp, NULL);
    h_remove(req->render_pass);
    return VK_SUCCESS;
}

static int32_t op_create_framebuffer(const uint8_t *pl, uint32_t len,
                                     uint8_t *resp, uint32_t *resp_len) {
    tw_req_create_framebuffer *req;
    VkDevice dev;
    PFN_vkCreateFramebuffer fn;
    VkFramebufferCreateInfo ci;
    VkImageView *views = NULL;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    uint32_t i;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_framebuffer *)pl;
    if (len < sizeof(*req) + req->attachments_len)
        return VK_ERROR_INITIALIZATION_FAILED;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&ci, &req->info, sizeof(ci));
    /* The render pass in the wire struct is an id; convert it. */
    if (req->info.renderPass != NULL) {
        VkRenderPass rp =
            h_get((tw_handle_t)(uintptr_t)req->info.renderPass,
                  H_RENDERPASS);
        if (rp == NULL) return VK_ERROR_DEVICE_LOST;
        ci.renderPass = rp;
    } else {
        ci.renderPass = VK_NULL_HANDLE;
    }
    if (req->info.attachmentCount > 0) {
        const tw_handle_t *ids =
            (const tw_handle_t *)(pl + sizeof(*req));
        views = malloc(req->info.attachmentCount * sizeof(*views));
        if (views == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (i = 0; i < req->info.attachmentCount; i++) {
            views[i] = h_get(ids[i], H_IMAGEVIEW);
            if (views[i] == NULL) {
                free(views);
                return VK_ERROR_DEVICE_LOST;
            }
        }
        ci.pAttachments = views;
    } else {
        ci.pAttachments = NULL;
    }
    fn = (PFN_vkCreateFramebuffer)resolve_device(dev, "vkCreateFramebuffer");
    if (fn == NULL) {
        free(views);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    r = fn(dev, &ci, NULL, &fb);
    free(views);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(fb, H_FRAMEBUFFER, req->device);
    if (id == 0) {
        PFN_vkDestroyFramebuffer dfn =
            (PFN_vkDestroyFramebuffer)resolve_device(dev, "vkDestroyFramebuffer");
        if (dfn != NULL) dfn(dev, fb, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_create_framebuffer *rp =
            (tw_resp_create_framebuffer *)resp;
        rp->framebuffer = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
}

static int32_t op_destroy_framebuffer(const uint8_t *pl, uint32_t len,
                                      uint8_t *resp, uint32_t *resp_len) {
    tw_req_destroy_framebuffer *req;
    VkDevice dev;
    VkFramebuffer fb;
    PFN_vkDestroyFramebuffer fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_destroy_framebuffer *)pl;
    dev = h_get(req->device, H_DEVICE);
    fb = h_get(req->framebuffer, H_FRAMEBUFFER);
    if (dev == NULL || fb == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkDestroyFramebuffer)resolve_device(dev, "vkDestroyFramebuffer");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, fb, NULL);
    h_remove(req->framebuffer);
    return VK_SUCCESS;
}

static int32_t op_create_shader_module(const uint8_t *pl, uint32_t len,
                                       uint8_t *resp, uint32_t *resp_len) {
    tw_req_create_shader_module *req;
    VkDevice dev;
    PFN_vkCreateShaderModule fn;
    VkShaderModuleCreateInfo ci;
    VkShaderModule sm = VK_NULL_HANDLE;
    VkResult r;
    uint64_t id;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_shader_module *)pl;
    if (len < sizeof(*req) + req->code_len)
        return VK_ERROR_INITIALIZATION_FAILED;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&ci, &req->info, sizeof(ci));
    ci.pCode = (const uint32_t *)(pl + sizeof(*req));
    fn = (PFN_vkCreateShaderModule)resolve_device(dev, "vkCreateShaderModule");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    r = fn(dev, &ci, NULL, &sm);
    if (r != VK_SUCCESS) return (int32_t)r;
    id = h_register(sm, H_SHADER, req->device);
    if (id == 0) {
        PFN_vkDestroyShaderModule dfn =
            (PFN_vkDestroyShaderModule)resolve_device(dev, "vkDestroyShaderModule");
        if (dfn != NULL) dfn(dev, sm, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    {
        tw_resp_create_shader_module *rp =
            (tw_resp_create_shader_module *)resp;
        rp->shader_module = id;
        *resp_len = sizeof(*rp);
    }
    return VK_SUCCESS;
}

static int32_t op_destroy_shader_module(const uint8_t *pl, uint32_t len,
                                        uint8_t *resp, uint32_t *resp_len) {
    tw_req_destroy_shader_module *req;
    VkDevice dev;
    VkShaderModule sm;
    PFN_vkDestroyShaderModule fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_destroy_shader_module *)pl;
    dev = h_get(req->device, H_DEVICE);
    sm = h_get(req->shader_module, H_SHADER);
    if (dev == NULL || sm == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkDestroyShaderModule)resolve_device(dev, "vkDestroyShaderModule");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, sm, NULL);
    h_remove(req->shader_module);
    return VK_SUCCESS;
}

/* Bump-allocate n bytes (align-byte aligned) from a scratch buffer.
 * Returns the pointer, or NULL when out of space. */
static void *scratch_alloc(uint8_t **sp, uint8_t *end, size_t n,
                           size_t align) {
    uintptr_t p = ((uintptr_t)*sp + (align - 1)) & ~(uintptr_t)(align - 1);
    if (p + n > (uintptr_t)end) return NULL;
    *sp = (uint8_t *)(p + n);
    return (void *)p;
}

static int32_t op_create_graphics_pipelines(const uint8_t *pl,
                                            uint32_t len, uint8_t *resp,
                                            uint32_t *resp_len) {
    tw_req_create_graphics_pipelines *req;
    VkDevice dev;
    PFN_vkCreateGraphicsPipelines fn;
    VkGraphicsPipelineCreateInfo *cis = NULL;
    VkPipeline *pipes = NULL;
    uint8_t *scratch = NULL;
    uint8_t *sp;
    uint8_t *scratch_end;
    const uint8_t *cur;
    const uint8_t *end;
    VkResult r;
    uint32_t i;
    uint32_t j;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_create_graphics_pipelines *)pl;
    if (req->create_info_count == 0 ||
        req->create_info_count > TURNIP_MAX_PIPELINES)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (len < sizeof(*req) + req->infos_len + req->sub_len)
        return VK_ERROR_INITIALIZATION_FAILED;
    dev = h_get(req->device, H_DEVICE);
    if (dev == NULL) return VK_ERROR_DEVICE_LOST;
    if (req->pipeline_cache != 0) return VK_ERROR_DEVICE_LOST;
    cis = malloc(req->create_info_count * sizeof(*cis));
    pipes = malloc(req->create_info_count * sizeof(*pipes));
    scratch = malloc(req->sub_len > 0 ? req->sub_len : 1);
    if (cis == NULL || pipes == NULL || scratch == NULL) goto fail;
    sp = scratch;
    scratch_end = scratch + (req->sub_len > 0 ? req->sub_len : 1);
    cur = pl + sizeof(*req);
    end = pl + len;
    /* The infos array is at cur; the sub-blob stream follows it. */
    {
        const uint8_t *infos = rd(&cur, end, req->infos_len);
        const uint8_t *stream;
        if (infos == NULL) goto fail;
        stream = cur;
        for (i = 0; i < req->create_info_count; i++) {
            const tw_VkGraphicsPipelineCreateInfo *ti =
                (const tw_VkGraphicsPipelineCreateInfo *)(infos +
                    i * sizeof(tw_VkGraphicsPipelineCreateInfo));
            VkGraphicsPipelineCreateInfo *ci = &cis[i];
            uint32_t mask = req->present_masks[i];
            memcpy(ci, ti, sizeof(*ci));
            /* Stages. */
            if (ti->stageCount > 0) {
                VkPipelineShaderStageCreateInfo *stages =
                    scratch_alloc(&sp, scratch_end,
                                  (size_t)ti->stageCount * sizeof(*stages),
                                  8);
                if (stages == NULL) goto fail;
                for (j = 0; j < ti->stageCount; j++) {
                    const tw_VkPipelineShaderStageCreateInfo *ts =
                        (const tw_VkPipelineShaderStageCreateInfo *)rd(
                            &stream, end,
                            sizeof(tw_VkPipelineShaderStageCreateInfo));
                    VkShaderModule mod;
                    if (ts == NULL) goto fail;
                    memcpy(&stages[j], ts, sizeof(stages[j]));
                    mod = h_get((tw_handle_t)(uintptr_t)ts->module, H_SHADER);
                    if (mod == NULL) goto fail;
                    stages[j].module = mod;
                    stages[j].pSpecializationInfo = NULL;
                    /* Name follows the stage structs; set it after
                     * reading all stage structs. */
                    stages[j].pName = NULL;
                }
                for (j = 0; j < ti->stageCount; j++) {
                    const uint8_t *name = stream;
                    while (stream < end && *stream != '\0') stream++;
                    if (stream >= end) goto fail;
                    stream++; /* past the NUL */
                    stages[j].pName = (const char *)name;
                }
                ci->pStages = stages;
            } else {
                ci->pStages = NULL;
            }
            /* Vertex input. */
            if (mask & TURNIP_PIPE_HAS_VERTEX_INPUT) {
                const tw_VkPipelineVertexInputStateCreateInfo *tv =
                    (const tw_VkPipelineVertexInputStateCreateInfo *)rd(
                        &stream, end,
                        sizeof(tw_VkPipelineVertexInputStateCreateInfo));
                VkPipelineVertexInputStateCreateInfo *v;
                if (tv == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, tv, sizeof(*v));
                if (tv->vertexBindingDescriptionCount > 0) {
                    const uint8_t *b = rd(&stream, end,
                        (size_t)tv->vertexBindingDescriptionCount *
                            sizeof(VkVertexInputBindingDescription));
                    if (b == NULL) goto fail;
                    v->pVertexBindingDescriptions =
                        (const VkVertexInputBindingDescription *)b;
                } else {
                    v->pVertexBindingDescriptions = NULL;
                }
                if (tv->vertexAttributeDescriptionCount > 0) {
                    const uint8_t *b = rd(&stream, end,
                        (size_t)tv->vertexAttributeDescriptionCount *
                            sizeof(VkVertexInputAttributeDescription));
                    if (b == NULL) goto fail;
                    v->pVertexAttributeDescriptions =
                        (const VkVertexInputAttributeDescription *)b;
                } else {
                    v->pVertexAttributeDescriptions = NULL;
                }
                ci->pVertexInputState = v;
            } else {
                ci->pVertexInputState = NULL;
            }
            /* Input assembly. */
            if (mask & TURNIP_PIPE_HAS_INPUT_ASSEMBLY) {
                const uint8_t *b = rd(&stream, end,
                    sizeof(VkPipelineInputAssemblyStateCreateInfo));
                VkPipelineInputAssemblyStateCreateInfo *v;
                if (b == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, b, sizeof(*v));
                ci->pInputAssemblyState = v;
            } else {
                ci->pInputAssemblyState = NULL;
            }
            /* Tessellation. */
            if (mask & TURNIP_PIPE_HAS_TESSELLATION) {
                const uint8_t *b = rd(&stream, end,
                    sizeof(VkPipelineTessellationStateCreateInfo));
                VkPipelineTessellationStateCreateInfo *v;
                if (b == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, b, sizeof(*v));
                ci->pTessellationState = v;
            } else {
                ci->pTessellationState = NULL;
            }
            /* Viewport. */
            if (mask & TURNIP_PIPE_HAS_VIEWPORT) {
                const tw_VkPipelineViewportStateCreateInfo *tv =
                    (const tw_VkPipelineViewportStateCreateInfo *)rd(
                        &stream, end,
                        sizeof(tw_VkPipelineViewportStateCreateInfo));
                VkPipelineViewportStateCreateInfo *v;
                if (tv == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, tv, sizeof(*v));
                if (tv->viewportCount > 0) {
                    const uint8_t *b = rd(&stream, end,
                        (size_t)tv->viewportCount * sizeof(VkViewport));
                    if (b == NULL) goto fail;
                    v->pViewports = (const VkViewport *)b;
                } else {
                    v->pViewports = NULL;
                }
                if (tv->scissorCount > 0) {
                    const uint8_t *b = rd(&stream, end,
                        (size_t)tv->scissorCount * sizeof(VkRect2D));
                    if (b == NULL) goto fail;
                    v->pScissors = (const VkRect2D *)b;
                } else {
                    v->pScissors = NULL;
                }
                ci->pViewportState = v;
            } else {
                ci->pViewportState = NULL;
            }
            /* Rasterization. */
            if (mask & TURNIP_PIPE_HAS_RASTERIZATION) {
                const uint8_t *b = rd(&stream, end,
                    sizeof(VkPipelineRasterizationStateCreateInfo));
                VkPipelineRasterizationStateCreateInfo *v;
                if (b == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, b, sizeof(*v));
                ci->pRasterizationState = v;
            } else {
                ci->pRasterizationState = NULL;
            }
            /* Multisample. */
            if (mask & TURNIP_PIPE_HAS_MULTISAMPLE) {
                const tw_VkPipelineMultisampleStateCreateInfo *tv =
                    (const tw_VkPipelineMultisampleStateCreateInfo *)rd(
                        &stream, end,
                        sizeof(tw_VkPipelineMultisampleStateCreateInfo));
                VkPipelineMultisampleStateCreateInfo *v;
                if (tv == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, tv, sizeof(*v));
                if (mask & TURNIP_PIPE_HAS_SAMPLE_MASK) {
                    uint32_t words = (tv->rasterizationSamples + 31u) / 32u;
                    const uint8_t *b = rd(&stream, end,
                        (size_t)words * sizeof(uint32_t));
                    if (b == NULL) goto fail;
                    v->pSampleMask = (const VkSampleMask *)b;
                } else {
                    v->pSampleMask = NULL;
                }
                ci->pMultisampleState = v;
            } else {
                ci->pMultisampleState = NULL;
            }
            /* Depth stencil. */
            if (mask & TURNIP_PIPE_HAS_DEPTH_STENCIL) {
                const uint8_t *b = rd(&stream, end,
                    sizeof(VkPipelineDepthStencilStateCreateInfo));
                VkPipelineDepthStencilStateCreateInfo *v;
                if (b == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, b, sizeof(*v));
                ci->pDepthStencilState = v;
            } else {
                ci->pDepthStencilState = NULL;
            }
            /* Color blend. */
            if (mask & TURNIP_PIPE_HAS_COLOR_BLEND) {
                const tw_VkPipelineColorBlendStateCreateInfo *tv =
                    (const tw_VkPipelineColorBlendStateCreateInfo *)rd(
                        &stream, end,
                        sizeof(tw_VkPipelineColorBlendStateCreateInfo));
                VkPipelineColorBlendStateCreateInfo *v;
                if (tv == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, tv, sizeof(*v));
                if (tv->attachmentCount > 0) {
                    const uint8_t *b = rd(&stream, end,
                        (size_t)tv->attachmentCount *
                            sizeof(VkPipelineColorBlendAttachmentState));
                    if (b == NULL) goto fail;
                    v->pAttachments =
                        (const VkPipelineColorBlendAttachmentState *)b;
                } else {
                    v->pAttachments = NULL;
                }
                ci->pColorBlendState = v;
            } else {
                ci->pColorBlendState = NULL;
            }
            /* Dynamic. */
            if (mask & TURNIP_PIPE_HAS_DYNAMIC) {
                const tw_VkPipelineDynamicStateCreateInfo *tv =
                    (const tw_VkPipelineDynamicStateCreateInfo *)rd(
                        &stream, end,
                        sizeof(tw_VkPipelineDynamicStateCreateInfo));
                VkPipelineDynamicStateCreateInfo *v;
                if (tv == NULL) goto fail;
                v = scratch_alloc(&sp, scratch_end, sizeof(*v), 8);
                if (v == NULL) goto fail;
                memcpy(v, tv, sizeof(*v));
                if (tv->dynamicStateCount > 0) {
                    const uint8_t *b = rd(&stream, end,
                        (size_t)tv->dynamicStateCount * sizeof(VkDynamicState));
                    if (b == NULL) goto fail;
                    v->pDynamicStates = (const VkDynamicState *)b;
                } else {
                    v->pDynamicStates = NULL;
                }
                ci->pDynamicState = v;
            } else {
                ci->pDynamicState = NULL;
            }
            /* Layout, render pass, base pipeline: handles from ids. */
            if (ti->layout != NULL) {
                VkPipelineLayout l =
                    h_get((tw_handle_t)(uintptr_t)ti->layout,
                          H_PIPELINELAYOUT);
                if (l == NULL) goto fail;
                ci->layout = l;
            } else {
                ci->layout = VK_NULL_HANDLE;
            }
            if (ti->renderPass != NULL) {
                VkRenderPass rp =
                    h_get((tw_handle_t)(uintptr_t)ti->renderPass,
                          H_RENDERPASS);
                if (rp == NULL) goto fail;
                ci->renderPass = rp;
            } else {
                ci->renderPass = VK_NULL_HANDLE;
            }
            if (ti->basePipelineHandle != NULL) {
                VkPipeline bp =
                    h_get((tw_handle_t)(uintptr_t)ti->basePipelineHandle,
                          H_PIPELINE);
                if (bp == NULL) goto fail;
                ci->basePipelineHandle = bp;
            } else {
                ci->basePipelineHandle = VK_NULL_HANDLE;
            }
        }
        if (stream != end) goto fail;
    }
    fn = (PFN_vkCreateGraphicsPipelines)resolve_device(
        dev, "vkCreateGraphicsPipelines");
    if (fn == NULL) goto fail;
    r = fn(dev, VK_NULL_HANDLE, req->create_info_count, cis, NULL, pipes);
    {
        tw_resp_create_graphics_pipelines *rp =
            (tw_resp_create_graphics_pipelines *)resp;
        tw_handle_t *ids = (tw_handle_t *)(resp + sizeof(*rp));
        uint32_t n = 0;
        rp->count = req->create_info_count;
        *resp_len = sizeof(*rp) +
                    (size_t)req->create_info_count * sizeof(tw_handle_t);
        for (i = 0; i < req->create_info_count; i++) {
            if (pipes[i] != VK_NULL_HANDLE) {
                uint64_t id = h_register(pipes[i], H_PIPELINE, req->device);
                ids[i] = id;
                if (id != 0) n++;
            } else {
                ids[i] = 0;
            }
        }
        (void)n;
    }
    free(cis);
    free(pipes);
    free(scratch);
    return (int32_t)r;
fail:
    free(cis);
    free(pipes);
    free(scratch);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static int32_t op_destroy_pipeline(const uint8_t *pl, uint32_t len,
                                   uint8_t *resp, uint32_t *resp_len) {
    tw_req_destroy_pipeline *req;
    VkDevice dev;
    VkPipeline pipe;
    PFN_vkDestroyPipeline fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_destroy_pipeline *)pl;
    dev = h_get(req->device, H_DEVICE);
    pipe = h_get(req->pipeline, H_PIPELINE);
    if (dev == NULL || pipe == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkDestroyPipeline)resolve_device(dev, "vkDestroyPipeline");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(dev, pipe, NULL);
    h_remove(req->pipeline);
    return VK_SUCCESS;
}

static int32_t op_cmd_begin_render_pass(const uint8_t *pl, uint32_t len,
                                        uint8_t *resp, uint32_t *resp_len) {
    tw_req_cmd_begin_render_pass *req;
    VkCommandBuffer cb;
    PFN_vkCmdBeginRenderPass fn;
    VkRenderPassBeginInfo bi;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_cmd_begin_render_pass *)pl;
    if (len < sizeof(*req) + req->clear_values_len)
        return VK_ERROR_INITIALIZATION_FAILED;
    cb = h_get(req->cmdbuf, H_CMDBUF);
    if (cb == NULL) return VK_ERROR_DEVICE_LOST;
    memcpy(&bi, &req->info, sizeof(bi));
    if (req->info.renderPass != NULL) {
        VkRenderPass rp =
            h_get((tw_handle_t)(uintptr_t)req->info.renderPass,
                  H_RENDERPASS);
        if (rp == NULL) return VK_ERROR_DEVICE_LOST;
        bi.renderPass = rp;
    } else {
        bi.renderPass = VK_NULL_HANDLE;
    }
    if (req->info.framebuffer != NULL) {
        VkFramebuffer fb =
            h_get((tw_handle_t)(uintptr_t)req->info.framebuffer,
                  H_FRAMEBUFFER);
        if (fb == NULL) return VK_ERROR_DEVICE_LOST;
        bi.framebuffer = fb;
    } else {
        bi.framebuffer = VK_NULL_HANDLE;
    }
    bi.pClearValues = req->clear_values_len > 0
                          ? (const VkClearValue *)(pl + sizeof(*req))
                          : NULL;
    fn = (PFN_vkCmdBeginRenderPass)resolve_device(dev_for(req->cmdbuf),
                                                  "vkCmdBeginRenderPass");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    return VK_SUCCESS;
}

static int32_t op_cmd_end_render_pass(const uint8_t *pl, uint32_t len,
                                      uint8_t *resp, uint32_t *resp_len) {
    tw_req_cmd_end_render_pass *req;
    VkCommandBuffer cb;
    PFN_vkCmdEndRenderPass fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_cmd_end_render_pass *)pl;
    cb = h_get(req->cmdbuf, H_CMDBUF);
    if (cb == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkCmdEndRenderPass)resolve_device(dev_for(req->cmdbuf),
                                                "vkCmdEndRenderPass");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(cb);
    return VK_SUCCESS;
}

static int32_t op_cmd_clear_attachments(const uint8_t *pl, uint32_t len,
                                        uint8_t *resp, uint32_t *resp_len) {
    tw_req_cmd_clear_attachments *req;
    VkCommandBuffer cb;
    PFN_vkCmdClearAttachments fn;
    const VkClearAttachment *atts = NULL;
    const VkClearRect *rects = NULL;
    const uint8_t *cur;
    const uint8_t *end;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_cmd_clear_attachments *)pl;
    cb = h_get(req->cmdbuf, H_CMDBUF);
    if (cb == NULL) return VK_ERROR_DEVICE_LOST;
    cur = pl + sizeof(*req);
    end = pl + len;
    if (req->attachment_count > 0) {
        const uint8_t *a = rd(&cur, end,
            (size_t)req->attachment_count * sizeof(VkClearAttachment));
        if (a == NULL) return VK_ERROR_INITIALIZATION_FAILED;
        atts = (const VkClearAttachment *)a;
    }
    if (req->rect_count > 0) {
        const uint8_t *a = rd(&cur, end,
            (size_t)req->rect_count * sizeof(VkClearRect));
        if (a == NULL) return VK_ERROR_INITIALIZATION_FAILED;
        rects = (const VkClearRect *)a;
    }
    if (cur != end) return VK_ERROR_INITIALIZATION_FAILED;
    fn = (PFN_vkCmdClearAttachments)resolve_device(dev_for(req->cmdbuf),
                                                   "vkCmdClearAttachments");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(cb, req->attachment_count, atts, req->rect_count, rects);
    return VK_SUCCESS;
}

static int32_t op_cmd_bind_pipeline(const uint8_t *pl, uint32_t len,
                                    uint8_t *resp, uint32_t *resp_len) {
    tw_req_cmd_bind_pipeline *req;
    VkCommandBuffer cb;
    VkPipeline pipe;
    PFN_vkCmdBindPipeline fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_cmd_bind_pipeline *)pl;
    cb = h_get(req->cmdbuf, H_CMDBUF);
    pipe = h_get(req->pipeline, H_PIPELINE);
    if (cb == NULL || pipe == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkCmdBindPipeline)resolve_device(dev_for(req->cmdbuf),
                                               "vkCmdBindPipeline");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(cb, (VkPipelineBindPoint)req->bind_point, pipe);
    return VK_SUCCESS;
}

static int32_t op_cmd_draw(const uint8_t *pl, uint32_t len,
                           uint8_t *resp, uint32_t *resp_len) {
    tw_req_cmd_draw *req;
    VkCommandBuffer cb;
    PFN_vkCmdDraw fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_cmd_draw *)pl;
    cb = h_get(req->cmdbuf, H_CMDBUF);
    if (cb == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkCmdDraw)resolve_device(dev_for(req->cmdbuf), "vkCmdDraw");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    fn(cb, req->vertex_count, req->instance_count, req->first_vertex,
       req->first_instance);
    return VK_SUCCESS;
}

static int32_t op_queue_submit(const uint8_t *pl, uint32_t len,
                               uint8_t *resp, uint32_t *resp_len) {
    tw_req_queue_submit *req;
    VkQueue queue;
    VkFence fence = VK_NULL_HANDLE;
    PFN_vkQueueSubmit fn;
    VkSubmitInfo *subs = NULL;
    void *allocs[32];
    int nallocs = 0;
    const uint8_t *cur;
    const uint8_t *end;
    VkResult r;
    uint32_t i;
    uint32_t j;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_queue_submit *)pl;
    queue = h_get(req->queue, H_QUEUE);
    if (queue == NULL) return VK_ERROR_DEVICE_LOST;
    if (req->fence != 0) {
        fence = h_get(req->fence, H_FENCE);
        if (fence == NULL) return VK_ERROR_DEVICE_LOST;
    }
    cur = pl + sizeof(*req);
    end = pl + len;
    if (req->submit_count > 0) {
        subs = malloc(req->submit_count * sizeof(*subs));
        if (subs == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (i = 0; i < req->submit_count; i++) {
            const tw_VkSubmitInfo *ts =
                (const tw_VkSubmitInfo *)rd(&cur, end, sizeof(*ts));
            VkSubmitInfo *s;
            if (ts == NULL) goto fail;
            s = &subs[i];
            memcpy(s, ts, sizeof(*s));
            if (ts->waitSemaphoreCount > 0) {
                VkSemaphore *sems = malloc(ts->waitSemaphoreCount *
                                          sizeof(*sems));
                const uint32_t *masks;
                if (sems == NULL) goto fail;
                if (nallocs < 32) allocs[nallocs++] = sems;
                for (j = 0; j < ts->waitSemaphoreCount; j++) {
                    const uint8_t *idb = rd(&cur, end, sizeof(tw_handle_t));
                    tw_handle_t id;
                    if (idb == NULL) goto fail;
                    memcpy(&id, idb, sizeof(id));
                    sems[j] = h_get(id, H_SEMAPHORE);
                    if (sems[j] == NULL) goto fail;
                }
                masks = (const uint32_t *)rd(
                    &cur, end,
                    (size_t)ts->waitSemaphoreCount * sizeof(uint32_t));
                if (masks == NULL) goto fail;
                s->pWaitSemaphores = sems;
                s->pWaitDstStageMask = masks;
            } else {
                s->pWaitSemaphores = NULL;
                s->pWaitDstStageMask = NULL;
            }
            if (ts->commandBufferCount > 0) {
                VkCommandBuffer *cbs = malloc(ts->commandBufferCount *
                                              sizeof(*cbs));
                if (cbs == NULL) goto fail;
                if (nallocs < 32) allocs[nallocs++] = cbs;
                for (j = 0; j < ts->commandBufferCount; j++) {
                    const uint8_t *idb = rd(&cur, end, sizeof(tw_handle_t));
                    tw_handle_t id;
                    if (idb == NULL) goto fail;
                    memcpy(&id, idb, sizeof(id));
                    cbs[j] = h_get(id, H_CMDBUF);
                    if (cbs[j] == NULL) goto fail;
                }
                s->pCommandBuffers = cbs;
            } else {
                s->pCommandBuffers = NULL;
            }
            if (ts->signalSemaphoreCount > 0) {
                VkSemaphore *sems = malloc(ts->signalSemaphoreCount *
                                          sizeof(*sems));
                if (sems == NULL) goto fail;
                if (nallocs < 32) allocs[nallocs++] = sems;
                for (j = 0; j < ts->signalSemaphoreCount; j++) {
                    const uint8_t *idb = rd(&cur, end, sizeof(tw_handle_t));
                    tw_handle_t id;
                    if (idb == NULL) goto fail;
                    memcpy(&id, idb, sizeof(id));
                    sems[j] = h_get(id, H_SEMAPHORE);
                    if (sems[j] == NULL) goto fail;
                }
                s->pSignalSemaphores = sems;
            } else {
                s->pSignalSemaphores = NULL;
            }
        }
    }
    if (cur != end) goto fail;
    fn = (PFN_vkQueueSubmit)resolve_device(dev_for(req->queue),
                                           "vkQueueSubmit");
    if (fn == NULL) goto fail;
    r = fn(queue, req->submit_count, subs, fence);
    for (i = 0; i < (uint32_t)nallocs; i++) free(allocs[i]);
    free(subs);
    return (int32_t)r;
fail:
    for (i = 0; i < (uint32_t)nallocs; i++) free(allocs[i]);
    free(subs);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static int32_t op_queue_wait_idle(const uint8_t *pl, uint32_t len,
                                  uint8_t *resp, uint32_t *resp_len) {
    tw_req_queue_wait_idle *req;
    VkQueue queue;
    PFN_vkQueueWaitIdle fn;
    (void)resp;
    (void)resp_len;
    if (len < sizeof(*req)) return VK_ERROR_INITIALIZATION_FAILED;
    req = (tw_req_queue_wait_idle *)pl;
    queue = h_get(req->queue, H_QUEUE);
    if (queue == NULL) return VK_ERROR_DEVICE_LOST;
    fn = (PFN_vkQueueWaitIdle)resolve_device(dev_for(req->queue),
                                            "vkQueueWaitIdle");
    if (fn == NULL) return VK_ERROR_INITIALIZATION_FAILED;
    return (int32_t)fn(queue);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */

static int32_t dispatch(uint32_t op, const uint8_t *pl, uint32_t len,
                        uint8_t *resp, uint32_t *resp_len) {
    *resp_len = 0;
    switch (op) {
        case TURNIP_OP_PING:
            return op_ping(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_INSTANCE:
            return op_create_instance(pl, len, resp, resp_len);
        case TURNIP_OP_VK_ENUMERATE_PHYSICAL_DEVICES:
            return op_enumerate_physical_devices(pl, len, resp, resp_len);
        case TURNIP_OP_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
            return op_get_queue_family_props(pl, len, resp, resp_len);
        case TURNIP_OP_VK_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES:
            return op_get_memory_props(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_DEVICE:
            return op_create_device(pl, len, resp, resp_len);
        case TURNIP_OP_VK_GET_DEVICE_QUEUE:
            return op_get_device_queue(pl, len, resp, resp_len);
        case TURNIP_OP_VK_DEVICE_WAIT_IDLE:
            return op_device_wait_idle(pl, len, resp, resp_len);
        case TURNIP_OP_VK_ALLOCATE_MEMORY:
            return op_allocate_memory(pl, len, resp, resp_len);
        case TURNIP_OP_VK_FREE_MEMORY:
            return op_free_memory(pl, len, resp, resp_len);
        case TURNIP_OP_VK_GET_BUFFER_MEMORY_REQUIREMENTS:
            return op_get_buffer_memory_requirements(pl, len, resp, resp_len);
        case TURNIP_OP_VK_GET_IMAGE_MEMORY_REQUIREMENTS:
            return op_get_image_memory_requirements(pl, len, resp, resp_len);
        case TURNIP_OP_VK_BIND_BUFFER_MEMORY:
            return op_bind_buffer_memory(pl, len, resp, resp_len);
        case TURNIP_OP_VK_BIND_IMAGE_MEMORY:
            return op_bind_image_memory(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_BUFFER:
            return op_create_buffer(pl, len, resp, resp_len);
        case TURNIP_OP_VK_DESTROY_BUFFER:
            return op_destroy_buffer(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_IMAGE:
            return op_create_image(pl, len, resp, resp_len);
        case TURNIP_OP_VK_DESTROY_IMAGE:
            return op_destroy_image(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_COMMAND_POOL:
            return op_create_command_pool(pl, len, resp, resp_len);
        case TURNIP_OP_VK_DESTROY_COMMAND_POOL:
            return op_destroy_command_pool(pl, len, resp, resp_len);
        case TURNIP_OP_VK_ALLOCATE_COMMAND_BUFFERS:
            return op_allocate_command_buffers(pl, len, resp, resp_len);
        case TURNIP_OP_VK_FREE_COMMAND_BUFFERS:
            return op_free_command_buffers(pl, len, resp, resp_len);
        case TURNIP_OP_VK_BEGIN_COMMAND_BUFFER:
            return op_begin_command_buffer(pl, len, resp, resp_len);
        case TURNIP_OP_VK_END_COMMAND_BUFFER:
            return op_end_command_buffer(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_RENDER_PASS:
            return op_create_render_pass(pl, len, resp, resp_len);
        case TURNIP_OP_VK_DESTROY_RENDER_PASS:
            return op_destroy_render_pass(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_FRAMEBUFFER:
            return op_create_framebuffer(pl, len, resp, resp_len);
        case TURNIP_OP_VK_DESTROY_FRAMEBUFFER:
            return op_destroy_framebuffer(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_SHADER_MODULE:
            return op_create_shader_module(pl, len, resp, resp_len);
        case TURNIP_OP_VK_DESTROY_SHADER_MODULE:
            return op_destroy_shader_module(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CREATE_GRAPHICS_PIPELINES:
            return op_create_graphics_pipelines(pl, len, resp, resp_len);
        case TURNIP_OP_VK_DESTROY_PIPELINE:
            return op_destroy_pipeline(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CMD_BEGIN_RENDER_PASS:
            return op_cmd_begin_render_pass(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CMD_END_RENDER_PASS:
            return op_cmd_end_render_pass(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CMD_CLEAR_ATTACHMENTS:
            return op_cmd_clear_attachments(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CMD_BIND_PIPELINE:
            return op_cmd_bind_pipeline(pl, len, resp, resp_len);
        case TURNIP_OP_VK_CMD_DRAW:
            return op_cmd_draw(pl, len, resp, resp_len);
        case TURNIP_OP_VK_QUEUE_SUBMIT:
            return op_queue_submit(pl, len, resp, resp_len);
        case TURNIP_OP_VK_QUEUE_WAIT_IDLE:
            return op_queue_wait_idle(pl, len, resp, resp_len);
        case TURNIP_OP_GET_PHYSICAL_DEVICE_NAME:
            return op_get_physical_device_name(pl, len, resp, resp_len);
        default:
            return VK_ERROR_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ */
/* Server loop                                                          */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static _Alignas(8) uint8_t g_req[TURNIP_MAX_PAYLOAD];
static _Alignas(8) uint8_t g_resp[sizeof(tw_resp_hdr) + TURNIP_MAX_PAYLOAD];

/* Serve one client until it disconnects. Returns 0 on a clean
 * disconnect, -1 on a protocol error. */
static int serve_client(int fd) {
    for (;;) {
        tw_req_hdr rh;
        uint32_t resp_len = 0;
        tw_resp_hdr *ph;
        int32_t result;
        if (g_stop) return 0;
        if (read_all(fd, (uint8_t *)&rh, sizeof(rh)) != 0) return 0;
        if (rh.magic != TURNIP_WIRE_MAGIC ||
            rh.op >= TURNIP_OP_COUNT ||
            rh.payload_len > TURNIP_MAX_PAYLOAD) {
            fprintf(stderr, "gpu-helper: bad request header\n");
            return -1;
        }
        if (rh.payload_len > 0) {
            if (read_all(fd, g_req, rh.payload_len) != 0) return 0;
        }
        log_msg("gpu-helper: op %u seq %u len %u\n", rh.op, rh.seq,
                rh.payload_len);
        result = dispatch(rh.op, g_req, rh.payload_len,
                          g_resp + sizeof(tw_resp_hdr), &resp_len);
        ph = (tw_resp_hdr *)g_resp;
        ph->magic = TURNIP_WIRE_MAGIC;
        ph->op = rh.op;
        ph->seq = rh.seq;
        ph->result = result;
        ph->payload_len = resp_len;
        if (write_all(fd, g_resp, sizeof(*ph) + resp_len) != 0) return 0;
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s --socket <path> [--mode host|device] "
            "[--icd <path>] [--verbose]\n",
            prog);
}

int main(int argc, char **argv) {
    const char *socket_path = "/tmp/turnip-gpu-helper.sock";
    const char *icd_path = NULL;
    int mode_icd = 0;
    int i;
    int listen_fd;
    struct sockaddr_un addr;
    struct sigaction sa;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "host") == 0) {
                mode_icd = 0;
            } else if (strcmp(m, "device") == 0) {
                mode_icd = 1;
            } else {
                fprintf(stderr, "gpu-helper: unknown mode %s\n", m);
                return 2;
            }
        } else if (strcmp(argv[i], "--icd") == 0 && i + 1 < argc) {
            icd_path = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "gpu-helper: unknown argument %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (vk_load(mode_icd, icd_path) != 0) return 1;
    fprintf(stderr, "gpu-helper: Vulkan loaded (%s mode)\n",
            mode_icd ? "device" : "host");

    g_socket_path = socket_path;
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("gpu-helper: socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "gpu-helper: socket path too long\n");
        return 1;
    }
    strcpy(addr.sun_path, socket_path);
    unlink(socket_path);
    if (bind(listen_fd, (struct sockaddr *)&addr,
             (socklen_t)(sizeof(addr.sun_family) + strlen(socket_path) + 1)) != 0) {
        perror("gpu-helper: bind");
        return 1;
    }
    if (listen(listen_fd, 8) != 0) {
        perror("gpu-helper: listen");
        return 1;
    }
    g_listen_fd = listen_fd;
    fprintf(stderr, "gpu-helper: listening on %s\n", socket_path);

    while (!g_stop) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("gpu-helper: accept");
            break;
        }
        log_msg("gpu-helper: client connected\n");
        serve_client(fd);
        close(fd);
        log_msg("gpu-helper: client disconnected\n");
    }

    close(listen_fd);
    unlink(socket_path);
    fprintf(stderr, "gpu-helper: shutting down\n");
    return 0;
}
