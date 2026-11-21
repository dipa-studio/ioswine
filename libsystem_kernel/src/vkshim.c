/*
 * vkshim.c: runtime-filled table of Vulkan entry points for ioswine.
 * See vkshim.h for the entry set and the loading contract.
 *
 * Style: freestanding C, no libc. No memset/strcpy: the struct is
 * zeroed and names are copied with small explicit loops, so this file
 * needs no libc symbols at all.
 */
#include "vkshim.h"

/* Zero a struct without memset. */
static void vkshim_zero(struct vk_shim *out) {
    unsigned char *p = (unsigned char *)out;
    size_t i;
    for (i = 0; i < sizeof(*out); i++) {
        p[i] = 0;
    }
}

/* Copy a name into a fixed-size slot without strcpy. Truncates with
 * NUL termination if the name does not fit. */
static void vkshim_copy_name(char *dst, const char *src, unsigned int dst_size) {
    unsigned int i = 0;
    if (dst_size == 0) {
        return;
    }
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Record one missing entry name in the struct. Names past the table
 * are dropped but still fail the load. */
static void vkshim_record_missing(struct vk_shim *out, const char *name) {
    if (out->missing_count < VKSHIM_MAX_MISSING) {
        vkshim_copy_name(out->missing_names[out->missing_count], name,
                         VKSHIM_MAX_NAME_LEN);
    }
    out->missing_count++;
}

/* Look one entry up and store it. Returns 0 on success, -1 when the
 * entry did not resolve (recorded in the struct). */
static int vkshim_load_one(vk_get_proc_addr_fn get_proc_addr,
                           const char *name, void **slot,
                           struct vk_shim *out) {
    void *fn = get_proc_addr(name);
    if (fn == NULL) {
        vkshim_record_missing(out, name);
        return -1;
    }
    *slot = fn;
    return 0;
}

int vkshim_load(vk_get_proc_addr_fn get_proc_addr, struct vk_shim *out) {
    int failed = 0;
    if (get_proc_addr == NULL || out == NULL) {
        return -1;
    }
    vkshim_zero(out);
    /* Every required entry is attempted even after a failure, so all
     * missing names are recorded, not just the first. The calls are
     * written out one by one on purpose: no static name table (see
     * ARCHITECTURE.md section 6; tables live in runtime-filled
     * structs, never in static initializers). */
    if (vkshim_load_one(get_proc_addr, "vkCreateInstance", &out->vk_create_instance, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkEnumeratePhysicalDevices", &out->vk_enumerate_physical_devices, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkGetPhysicalDeviceQueueFamilyProperties", &out->vk_get_physical_device_queue_family_properties, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkGetPhysicalDeviceMemoryProperties", &out->vk_get_physical_device_memory_properties, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCreateDevice", &out->vk_create_device, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkGetDeviceQueue", &out->vk_get_device_queue, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkDeviceWaitIdle", &out->vk_device_wait_idle, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkAllocateMemory", &out->vk_allocate_memory, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkFreeMemory", &out->vk_free_memory, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkGetBufferMemoryRequirements", &out->vk_get_buffer_memory_requirements, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkGetImageMemoryRequirements", &out->vk_get_image_memory_requirements, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkBindBufferMemory", &out->vk_bind_buffer_memory, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkBindImageMemory", &out->vk_bind_image_memory, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCreateBuffer", &out->vk_create_buffer, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkDestroyBuffer", &out->vk_destroy_buffer, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCreateImage", &out->vk_create_image, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkDestroyImage", &out->vk_destroy_image, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCreateCommandPool", &out->vk_create_command_pool, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkDestroyCommandPool", &out->vk_destroy_command_pool, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkAllocateCommandBuffers", &out->vk_allocate_command_buffers, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkFreeCommandBuffers", &out->vk_free_command_buffers, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkBeginCommandBuffer", &out->vk_begin_command_buffer, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkEndCommandBuffer", &out->vk_end_command_buffer, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCreateRenderPass", &out->vk_create_render_pass, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkDestroyRenderPass", &out->vk_destroy_render_pass, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCreateFramebuffer", &out->vk_create_framebuffer, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkDestroyFramebuffer", &out->vk_destroy_framebuffer, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCreateShaderModule", &out->vk_create_shader_module, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkDestroyShaderModule", &out->vk_destroy_shader_module, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCreateGraphicsPipelines", &out->vk_create_graphics_pipelines, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkDestroyPipeline", &out->vk_destroy_pipeline, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCmdBeginRenderPass", &out->vk_cmd_begin_render_pass, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCmdEndRenderPass", &out->vk_cmd_end_render_pass, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCmdClearAttachments", &out->vk_cmd_clear_attachments, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCmdBindPipeline", &out->vk_cmd_bind_pipeline, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkCmdDraw", &out->vk_cmd_draw, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkQueueSubmit", &out->vk_queue_submit, out) != 0) {
        failed = 1;
    }
    if (vkshim_load_one(get_proc_addr, "vkQueueWaitIdle", &out->vk_queue_wait_idle, out) != 0) {
        failed = 1;
    }
    return failed ? -1 : 0;
}
