/*
 * vkshim.h: runtime-filled table of Vulkan entry points for ioswine
 *
 *
 * A runtime-filled table of Vulkan entry points. No static
 * initializers: vkshim_load() fills the table by looking each entry
 * point up by name through the caller-supplied get_proc_addr callback.
 * In the GPU helper process get_proc_addr is dlsym on the dlopen'd
 * Turnip ICD; in the lsk it returns marshaling stubs that talk to the
 * helper over a socket (future work, staged plan); in the host test
 * it returns stubs.
 *
 * The set is minimal but sufficient for a clear-color render pass,
 * grouped by what the pass needs:
 *   instance: create one, enumerate physical devices, query queue
 *     families and memory properties;
 *   device: create a logical device, get a queue, wait idle;
 *   memory: allocate/free, query requirements, bind to buffers/images;
 *   buffer/image: create/destroy the vertex buffer and the
 *     memfd-backed render target image;
 *   command pool/buffers: allocate, begin/end, free;
 *   render pass/framebuffer/pipeline/shader: the pass with a
 *     clear-color load op, its framebuffer, the passthrough shader
 *     module and the graphics pipeline;
 *   commands: begin/end render pass, clear attachments, bind
 *     pipeline, draw;
 *   submit/wait: queue submit and queue wait idle.
 * Deliberately NOT included: swapchain/presentation (that is task
 * 010's problem; the render target here is an IOSurface-backed
 * memfd image, not a swapchain image), vkGetInstanceProcAddr /
 * vkGetDeviceProcAddr (those are the loader's mechanism, not the
 * pass's entries; the caller resolves device-level entries through
 * the same get_proc_addr callback in this skeleton).
 *
 * All entries are void *: the skeleton never calls them, because it
 * has no VkDevice/VkQueue handles yet. metaldev_commit() checks that
 * the submit-path entries (vk_queue_submit, vk_queue_wait_idle) are
 * present when a shim is attached and records the submission; the
 * real calls with real handles land with device/queue creation.
 * vkshim_load() records every entry that fails to resolve in
 * missing_names so the caller can log it.
 *
 * Style: freestanding C, no libc.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef void *(*vk_get_proc_addr_fn)(const char *name);

/* Max missing entries recorded, and max entry-name length recorded.
 * The longest entry name below is
 * "vkGetPhysicalDeviceQueueFamilyProperties" (40 chars + NUL). */
#define VKSHIM_MAX_MISSING   8
#define VKSHIM_MAX_NAME_LEN  48

/* Function pointers, filled by vkshim_load(). Keep the set minimal but
   sufficient for a clear-color render pass: instance/device/queue,
   buffer/image/memory, command pool/buffers, render pass/framebuffer/
   pipeline, shader module, submit/wait. */
struct vk_shim {
    /* Instance. */
    void *vk_create_instance;
    void *vk_enumerate_physical_devices;
    void *vk_get_physical_device_queue_family_properties;
    void *vk_get_physical_device_memory_properties;
    /* Device / queue. */
    void *vk_create_device;
    void *vk_get_device_queue;
    void *vk_device_wait_idle;
    /* Memory. */
    void *vk_allocate_memory;
    void *vk_free_memory;
    void *vk_get_buffer_memory_requirements;
    void *vk_get_image_memory_requirements;
    void *vk_bind_buffer_memory;
    void *vk_bind_image_memory;
    /* Buffer / image. */
    void *vk_create_buffer;
    void *vk_destroy_buffer;
    void *vk_create_image;
    void *vk_destroy_image;
    /* Command pool / buffers. */
    void *vk_create_command_pool;
    void *vk_destroy_command_pool;
    void *vk_allocate_command_buffers;
    void *vk_free_command_buffers;
    void *vk_begin_command_buffer;
    void *vk_end_command_buffer;
    /* Render pass / framebuffer / pipeline / shader module. */
    void *vk_create_render_pass;
    void *vk_destroy_render_pass;
    void *vk_create_framebuffer;
    void *vk_destroy_framebuffer;
    void *vk_create_shader_module;
    void *vk_destroy_shader_module;
    void *vk_create_graphics_pipelines;
    void *vk_destroy_pipeline;
    /* Commands. */
    void *vk_cmd_begin_render_pass;
    void *vk_cmd_end_render_pass;
    void *vk_cmd_clear_attachments;
    void *vk_cmd_bind_pipeline;
    void *vk_cmd_draw;
    /* Submit / wait. */
    void *vk_queue_submit;
    void *vk_queue_wait_idle;

    /* Missing-entry record, filled by vkshim_load(). missing_count is
     * the number of required entries that did not resolve; their names
     * are in missing_names (up to VKSHIM_MAX_MISSING; further missing
     * names are not recorded but still fail the load). */
    unsigned int missing_count;
    char missing_names[VKSHIM_MAX_MISSING][VKSHIM_MAX_NAME_LEN];
};

/* Fill `out` by looking each entry point up by name through
   `get_proc_addr`. Returns 0 if every required entry resolved, -1
   otherwise (with the missing names recorded in the struct for
   logging). Every required entry is attempted even after a failure,
   so all missing names are recorded, not just the first. */
int vkshim_load(vk_get_proc_addr_fn get_proc_addr, struct vk_shim *out);
