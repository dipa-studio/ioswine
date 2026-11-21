/*
 * turnip_test.c: host test for the vkshim -> turnip stub -> socket ->
 * gpu-helper -> real Vulkan round-trip.
 *
 * Starts the helper in host mode (Mesa llvmpipe via the system
 * libvulkan), loads the vkshim through turnip_get_proc_addr, asserts
 * all 38 entries resolved, then drives a real instance, physical
 * device enumeration, queue family and memory queries, a logical
 * device, a queue, a buffer and a memory allocation through the
 * stubs, and prints the physical device name.
 *
 * Usage: turnip_test <path-to-gpu-helper>
 *
 * Exit code 0 on pass, non-zero on any failure. The helper is killed
 * (SIGTERM) at the end; a non-zero helper exit status is a failure.
 *
 * Layout checks: the tw_ structs must be layout-identical to the
 * official Vulkan structs; the _Static_asserts below catch drift
 * against the installed headers (the helper has the same checks
 * against the real structs it uses).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <vulkan/vulkan.h>

#include "turnip.h"
#include "turnip_vktypes.h"
#include "vkshim.h" /* from the src tree, via -I */

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: " __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

#define CHECK_VK(r, what) CHECK((r) == VK_SUCCESS, "%s: VkResult %d", \
                                (what), (int)(r))

/* Layout: tw_ structs must match the official Vulkan structs. */
_Static_assert(sizeof(tw_VkApplicationInfo) == sizeof(VkApplicationInfo),
               "tw_VkApplicationInfo size");
_Static_assert(sizeof(tw_VkInstanceCreateInfo) == sizeof(VkInstanceCreateInfo),
               "tw_VkInstanceCreateInfo size");
_Static_assert(sizeof(tw_VkDeviceQueueCreateInfo) ==
               sizeof(VkDeviceQueueCreateInfo),
               "tw_VkDeviceQueueCreateInfo size");
_Static_assert(sizeof(tw_VkDeviceCreateInfo) == sizeof(VkDeviceCreateInfo),
               "tw_VkDeviceCreateInfo size");
_Static_assert(sizeof(tw_VkQueueFamilyProperties) ==
               sizeof(VkQueueFamilyProperties),
               "tw_VkQueueFamilyProperties size");
_Static_assert(sizeof(tw_VkPhysicalDeviceMemoryProperties) ==
               sizeof(VkPhysicalDeviceMemoryProperties),
               "tw_VkPhysicalDeviceMemoryProperties size");
_Static_assert(sizeof(tw_VkMemoryAllocateInfo) == sizeof(VkMemoryAllocateInfo),
               "tw_VkMemoryAllocateInfo size");
_Static_assert(sizeof(tw_VkBufferCreateInfo) == sizeof(VkBufferCreateInfo),
               "tw_VkBufferCreateInfo size");
_Static_assert(sizeof(tw_VkMemoryRequirements) == sizeof(VkMemoryRequirements),
               "tw_VkMemoryRequirements size");
_Static_assert(sizeof(tw_VkPhysicalDeviceFeatures) ==
               sizeof(VkPhysicalDeviceFeatures),
               "tw_VkPhysicalDeviceFeatures size");
_Static_assert(sizeof(tw_VkRenderPassCreateInfo) ==
               sizeof(VkRenderPassCreateInfo),
               "tw_VkRenderPassCreateInfo size");
_Static_assert(sizeof(tw_VkFramebufferCreateInfo) ==
               sizeof(VkFramebufferCreateInfo),
               "tw_VkFramebufferCreateInfo size");
_Static_assert(sizeof(tw_VkShaderModuleCreateInfo) ==
               sizeof(VkShaderModuleCreateInfo),
               "tw_VkShaderModuleCreateInfo size");
_Static_assert(sizeof(tw_VkGraphicsPipelineCreateInfo) ==
               sizeof(VkGraphicsPipelineCreateInfo),
               "tw_VkGraphicsPipelineCreateInfo size");
_Static_assert(sizeof(tw_VkCommandBufferAllocateInfo) ==
               sizeof(VkCommandBufferAllocateInfo),
               "tw_VkCommandBufferAllocateInfo size");
_Static_assert(sizeof(tw_VkSubmitInfo) == sizeof(VkSubmitInfo),
               "tw_VkSubmitInfo size");
_Static_assert(offsetof(tw_VkInstanceCreateInfo, sType) ==
               offsetof(VkInstanceCreateInfo, sType),
               "tw_VkInstanceCreateInfo sType offset");
_Static_assert(offsetof(tw_VkInstanceCreateInfo, pApplicationInfo) ==
               offsetof(VkInstanceCreateInfo, pApplicationInfo),
               "tw_VkInstanceCreateInfo pApplicationInfo offset");
_Static_assert(offsetof(tw_VkDeviceCreateInfo, pQueueCreateInfos) ==
               offsetof(VkDeviceCreateInfo, pQueueCreateInfos),
               "tw_VkDeviceCreateInfo pQueueCreateInfos offset");
_Static_assert(offsetof(tw_VkPhysicalDeviceFeatures,
                        robustBufferAccess) ==
               offsetof(VkPhysicalDeviceFeatures, robustBufferAccess),
               "tw_VkPhysicalDeviceFeatures first member offset");
_Static_assert(offsetof(tw_VkPhysicalDeviceFeatures,
                        sparseResidencyAliased) ==
               offsetof(VkPhysicalDeviceFeatures, sparseResidencyAliased),
               "tw_VkPhysicalDeviceFeatures last member offset");

/* Wait up to `secs` seconds for a file to appear. */
static int wait_for_file(const char *path, int secs) {
    int i;
    struct stat st;
    for (i = 0; i < secs * 50; i++) {
        if (stat(path, &st) == 0) return 0;
        usleep(20000);
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *helper_path;
    char sock_path[256];
    pid_t helper_pid;
    struct vk_shim shim;
    int load_rc;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice *phys = NULL;
    uint32_t phys_count = 0;
    VkResult r;
    uint32_t i;
    int status = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-gpu-helper>\n", argv[0]);
        return 2;
    }
    helper_path = argv[1];
    snprintf(sock_path, sizeof(sock_path), "/tmp/turnip-test-%d.sock",
             (int)getpid());

    /* Start the helper in host mode. */
    helper_pid = fork();
    if (helper_pid == 0) {
        execl(helper_path, helper_path, "--socket", sock_path,
              "--mode", "host", (char *)NULL);
        perror("execl gpu-helper");
        _exit(127);
    }
    if (helper_pid < 0) {
        perror("fork");
        return 1;
    }
    if (wait_for_file(sock_path, 5) != 0) {
        fprintf(stderr, "FAIL: helper socket %s did not appear\n",
                sock_path);
        kill(helper_pid, SIGTERM);
        waitpid(helper_pid, &status, 0);
        return 1;
    }

    /* Load the shim through the turnip stubs. */
    if (turnip_init(sock_path) != 0) {
        fprintf(stderr, "FAIL: turnip_init failed (last_error=%d)\n",
                turnip_last_error());
        kill(helper_pid, SIGTERM);
        waitpid(helper_pid, &status, 0);
        return 1;
    }
    load_rc = vkshim_load(turnip_get_proc_addr, &shim);
    CHECK(load_rc == 0, "vkshim_load returned %d", load_rc);
    CHECK(shim.missing_count == 0, "missing_count=%u",
          shim.missing_count);
    for (i = 0; i < shim.missing_count; i++) {
        fprintf(stderr, "  missing: %s\n", shim.missing_names[i]);
    }
    if (g_failures > 0) goto out;
    printf("turnip_test: all 38 vkshim entries resolved\n");

    /* Instance. */
    {
        VkApplicationInfo app;
        VkInstanceCreateInfo ci;
        memset(&app, 0, sizeof(app));
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "turnip_test";
        app.applicationVersion = 1;
        app.pEngineName = "turnip_test";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_0;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = 0;
        ci.enabledLayerCount = 0;
        r = ((PFN_vkCreateInstance)shim.vk_create_instance)(
            &ci, NULL, &inst);
        CHECK_VK(r, "vkCreateInstance");
        if (g_failures > 0) goto out;
        printf("turnip_test: instance created\n");
    }

    /* Enumerate physical devices. */
    r = ((PFN_vkEnumeratePhysicalDevices)
         shim.vk_enumerate_physical_devices)(inst, &phys_count, NULL);
    CHECK_VK(r, "vkEnumeratePhysicalDevices (count)");
    CHECK(phys_count > 0, "no physical devices");
    if (g_failures > 0) goto out;
    phys = malloc(phys_count * sizeof(*phys));
    if (phys == NULL) {
        fprintf(stderr, "FAIL: malloc\n");
        g_failures++;
        goto out;
    }
    r = ((PFN_vkEnumeratePhysicalDevices)
         shim.vk_enumerate_physical_devices)(inst, &phys_count, phys);
    CHECK_VK(r, "vkEnumeratePhysicalDevices (list)");
    if (g_failures > 0) goto out;
    printf("turnip_test: %u physical device(s)\n", phys_count);

    /* Physical device name through the helper op. */
    {
        char name[256];
        int rc = turnip_physical_device_name(
            (tw_VkPhysicalDevice)phys[0], name, sizeof(name));
        CHECK(rc == 0, "turnip_physical_device_name failed (rc=%d)", rc);
        if (rc == 0) {
            printf("turnip_test: physical device: %s\n", name);
        }
    }

    /* Queue families and memory properties. */
    {
        uint32_t qf_count = 0;
        VkQueueFamilyProperties *qf = NULL;
        VkPhysicalDeviceMemoryProperties mem;
        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)
         shim.vk_get_physical_device_queue_family_properties)(
            phys[0], &qf_count, NULL);
        CHECK(qf_count > 0, "no queue families");
        if (g_failures > 0) goto out;
        qf = malloc(qf_count * sizeof(*qf));
        if (qf == NULL) {
            fprintf(stderr, "FAIL: malloc\n");
            g_failures++;
            goto out;
        }
        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)
         shim.vk_get_physical_device_queue_family_properties)(
            phys[0], &qf_count, qf);
        printf("turnip_test: %u queue families; family 0 flags 0x%x, "
               "queues %u\n",
               qf_count, (unsigned)qf[0].queueFlags,
               (unsigned)qf[0].queueCount);
        free(qf);
        ((PFN_vkGetPhysicalDeviceMemoryProperties)
         shim.vk_get_physical_device_memory_properties)(phys[0], &mem);
        CHECK(mem.memoryTypeCount > 0, "no memory types");
        CHECK(mem.memoryHeapCount > 0, "no memory heaps");
        printf("turnip_test: %u memory types, %u heaps\n",
               (unsigned)mem.memoryTypeCount,
               (unsigned)mem.memoryHeapCount);
        if (g_failures > 0) goto out;
    }

    /* Logical device + queue + buffer + memory + wait idle. */
    {
        VkDevice dev = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci;
        VkDeviceCreateInfo dci;
        VkBufferCreateInfo bci;
        VkMemoryRequirements reqs;
        VkMemoryAllocateInfo mai;
        VkPhysicalDeviceMemoryProperties mprops;
        uint32_t mem_type = 0;
        uint32_t t;

        memset(&qci, 0, sizeof(qci));
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        memset(&dci, 0, sizeof(dci));
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 0;
        dci.enabledLayerCount = 0;
        r = ((PFN_vkCreateDevice)shim.vk_create_device)(phys[0], &dci,
                                                        NULL, &dev);
        CHECK_VK(r, "vkCreateDevice");
        if (g_failures > 0) goto out;
        printf("turnip_test: logical device created\n");

        ((PFN_vkGetDeviceQueue)shim.vk_get_device_queue)(dev, 0, 0,
                                                         &queue);
        CHECK(queue != VK_NULL_HANDLE, "vkGetDeviceQueue returned NULL");
        if (g_failures > 0) goto out;
        printf("turnip_test: queue obtained\n");

        memset(&bci, 0, sizeof(bci));
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = 256;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        r = ((PFN_vkCreateBuffer)shim.vk_create_buffer)(dev, &bci, NULL,
                                                        &buf);
        CHECK_VK(r, "vkCreateBuffer");
        if (g_failures > 0) goto out;

        ((PFN_vkGetBufferMemoryRequirements)
         shim.vk_get_buffer_memory_requirements)(dev, buf, &reqs);
        CHECK(reqs.size >= 256, "buffer requirements size %llu",
              (unsigned long long)reqs.size);
        if (g_failures > 0) goto out;

        ((PFN_vkGetPhysicalDeviceMemoryProperties)
         shim.vk_get_physical_device_memory_properties)(phys[0], &mprops);
        mem_type = UINT32_MAX;
        for (t = 0; t < mprops.memoryTypeCount; t++) {
            if ((reqs.memoryTypeBits & (1u << t)) == 0) continue;
            if ((mprops.memoryTypes[t].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                mem_type = t;
                break;
            }
        }
        if (mem_type == UINT32_MAX) {
            for (t = 0; t < mprops.memoryTypeCount; t++) {
                if (reqs.memoryTypeBits & (1u << t)) {
                    mem_type = t;
                    break;
                }
            }
        }
        CHECK(mem_type != UINT32_MAX, "no suitable memory type");
        if (g_failures > 0) goto out;

        memset(&mai, 0, sizeof(mai));
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = reqs.size;
        mai.memoryTypeIndex = mem_type;
        r = ((PFN_vkAllocateMemory)shim.vk_allocate_memory)(dev, &mai,
                                                           NULL, &mem);
        CHECK_VK(r, "vkAllocateMemory");
        if (g_failures > 0) goto out;

        r = ((PFN_vkBindBufferMemory)shim.vk_bind_buffer_memory)(
            dev, buf, mem, 0);
        CHECK_VK(r, "vkBindBufferMemory");
        if (g_failures > 0) goto out;
        printf("turnip_test: buffer %llu bytes bound to memory type %u\n",
               (unsigned long long)reqs.size, (unsigned)mem_type);

        r = ((PFN_vkDeviceWaitIdle)shim.vk_device_wait_idle)(dev);
        CHECK_VK(r, "vkDeviceWaitIdle");
        if (g_failures > 0) goto out;
        printf("turnip_test: device wait idle OK\n");

        /* Cleanup of what the 38-entry set can destroy. There is
         * no vkDestroyDevice / vkDestroyInstance in the set, so the
         * device and instance live until the helper exits. */
        ((PFN_vkDestroyBuffer)shim.vk_destroy_buffer)(dev, buf, NULL);
        ((PFN_vkFreeMemory)shim.vk_free_memory)(dev, mem, NULL);
        printf("turnip_test: buffer freed, memory freed\n");
    }

out:
    free(phys);
    turnip_shutdown();
    kill(helper_pid, SIGTERM);
    if (waitpid(helper_pid, &status, 0) < 0) {
        perror("waitpid");
        g_failures++;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: helper exited abnormally (status %d)\n",
                status);
        g_failures++;
    }
    unlink(sock_path);
    if (g_failures == 0) {
        printf("turnip_test: PASS\n");
        return 0;
    }
    printf("turnip_test: FAIL (%d failures)\n", g_failures);
    return 1;
}
