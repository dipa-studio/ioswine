/* metaldev_test.c - host unit test for the metaldev skeleton.
 *
 * Plain cc, no NDK. The library itself uses no libc; this harness does
 * (stdio, string) only to drive it and to report results. The mock
 * ObjC runtime (mock_objc.h) stands in for the real runtime, and a
 * stub get_proc_addr stands in for the Turnip ICD.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -O1 -I../../libsystem_kernel/src \
 *      -o metaldev_test metaldev_test.c \
 *      ../../libsystem_kernel/src/metaldev.c \
 *      ../../libsystem_kernel/src/vkshim.c
 *   ./metaldev_test
 *
 * Sanitizers:
 *   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -I../../libsystem_kernel/src \
 *      -o metaldev_test_asan metaldev_test.c \
 *      ../../libsystem_kernel/src/metaldev.c \
 *      ../../libsystem_kernel/src/vkshim.c
 *   ./metaldev_test_asan
 */
#include <stdio.h>
#include <string.h>

#include "mock_objc.h"
#include "vkshim.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (cond) { \
        printf("PASS %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } else { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_METHOD(cls, sel) do { \
    g_checks++; \
    if (mock_lookup_method((cls), (sel)) != NULL) { \
        printf("PASS %s:%d: method %s registered\n", __FILE__, __LINE__, (sel)); \
    } else { \
        g_failures++; \
        printf("FAIL %s:%d: method %s missing\n", __FILE__, __LINE__, (sel)); \
    } \
} while (0)

/* Stub Vulkan get_proc_addr: resolves every name to one stub function. */
static int stub_vk_fn(void) {
    return 0;
}

static void *stub_get_proc_addr(const char *name) {
    (void)name;
    return (void *)stub_vk_fn;
}

/* Same, but vkQueueSubmit does not resolve (vkshim_load failure path). */
static void *missing_submit_get_proc_addr(const char *name) {
    if (strcmp(name, "vkQueueSubmit") == 0) {
        return NULL;
    }
    return (void *)stub_vk_fn;
}

int main(void) {
    struct mtl_objc_runtime rt;
    struct vk_shim shim;
    struct vk_shim bad_shim;
    mtl_device_t dev;
    struct mtl_device *dev_s;
    mtl_queue_t q;
    mtl_queue_t q2;
    mtl_buffer_t buf;
    mtl_texture_t tex;
    mtl_pipeline_t pipe;
    mtl_cmdbuf_t cb;
    mtl_cmdbuf_t cb2;
    mtl_cmdbuf_t cb3;
    struct mtl_cmdbuf *cb_s;
    void *cls_device;
    void *cls_queue;
    void *cls_cmdbuf;
    void *cls_encoder;
    void *cls_buffer;
    void *cls_texture;
    void *cls_pipeline;

    mock_reset();

    /* metaldev_init() is a stub that fails gracefully. */
    CHECK(metaldev_init() == -1);
    CHECK(metaldev_default_device() == NULL);
    CHECK(metaldev_init_with_runtime(NULL) == -1);

    rt = mock_runtime();
    CHECK(metaldev_init_with_runtime(&rt) == 0);
    /* Idempotent: a second init is a no-op success, no duplicate classes. */
    CHECK(metaldev_init_with_runtime(&rt) == 0);
    CHECK(mock_class_count() == 7);

    /* All seven classes exist, and only registered ones are found. */
    cls_device = mock_get_class("IOSwineMTLDevice");
    cls_queue = mock_get_class("IOSwineMTLCommandQueue");
    cls_cmdbuf = mock_get_class("IOSwineMTLCommandBuffer");
    cls_encoder = mock_get_class("IOSwineMTLRenderCommandEncoder");
    cls_buffer = mock_get_class("IOSwineMTLBuffer");
    cls_texture = mock_get_class("IOSwineMTLTexture");
    cls_pipeline = mock_get_class("IOSwineMTLRenderPipelineState");
    CHECK(cls_device != NULL);
    CHECK(cls_queue != NULL);
    CHECK(cls_cmdbuf != NULL);
    CHECK(cls_encoder != NULL);
    CHECK(cls_buffer != NULL);
    CHECK(cls_texture != NULL);
    CHECK(cls_pipeline != NULL);
    CHECK(mock_get_class("IOSwineMTLNoSuchClass") == NULL);

    /* Every expected method is registered on its class. */
    CHECK_METHOD(cls_device, "newCommandQueue");
    CHECK_METHOD(cls_device, "newBufferWithLength:options:");
    CHECK_METHOD(cls_device, "newTextureWithMemfd:width:height:pixelFormat:");
    CHECK_METHOD(cls_device, "newRenderPipelineStateWithVertexFunction:fragmentFunction:");
    CHECK_METHOD(cls_queue, "commandBuffer");
    CHECK_METHOD(cls_cmdbuf, "commit");
    CHECK_METHOD(cls_cmdbuf, "renderCommandEncoderWithDescriptor:");
    CHECK_METHOD(cls_encoder, "setRenderPipelineState:");
    CHECK_METHOD(cls_encoder, "setClearColor:");
    CHECK_METHOD(cls_encoder, "drawPrimitives:vertexStart:vertexCount:");
    CHECK_METHOD(cls_encoder, "endEncoding");
    CHECK_METHOD(cls_buffer, "contents");
    CHECK_METHOD(cls_buffer, "length");
    CHECK_METHOD(cls_texture, "width");
    CHECK_METHOD(cls_texture, "height");
    CHECK_METHOD(cls_texture, "pixelFormat");
    CHECK_METHOD(cls_pipeline, "vertexFunction");
    CHECK_METHOD(cls_pipeline, "fragmentFunction");

    /* Default device. */
    dev = metaldev_default_device();
    CHECK(dev != NULL);
    dev_s = (struct mtl_device *)dev;
    CHECK(dev_s->vk == NULL);
    CHECK(dev_s->device_index == 0);

    /* vkshim_load: all entries resolve through the stub. */
    CHECK(vkshim_load(stub_get_proc_addr, &shim) == 0);
    CHECK(shim.missing_count == 0);
    CHECK(shim.vk_create_instance != NULL);
    CHECK(shim.vk_create_device != NULL);
    CHECK(shim.vk_create_render_pass != NULL);
    CHECK(shim.vk_cmd_draw != NULL);
    CHECK(shim.vk_queue_submit != NULL);
    CHECK(shim.vk_queue_wait_idle != NULL);
    /* One missing entry fails the load and is recorded by name. */
    CHECK(vkshim_load(missing_submit_get_proc_addr, &bad_shim) == -1);
    CHECK(bad_shim.missing_count == 1);
    CHECK(strcmp(bad_shim.missing_names[0], "vkQueueSubmit") == 0);
    CHECK(bad_shim.vk_queue_submit == NULL);
    CHECK(bad_shim.vk_create_instance != NULL);
    /* NULL arguments fail. */
    CHECK(vkshim_load(NULL, &shim) == -1);
    CHECK(vkshim_load(stub_get_proc_addr, NULL) == -1);

    /* Queue, buffer, texture, pipeline creation. */
    q = metaldev_new_command_queue(dev);
    CHECK(q != NULL);
    CHECK(metaldev_new_command_queue(NULL) == NULL);
    buf = metaldev_new_buffer(dev, 256, 0);
    CHECK(buf != NULL);
    CHECK(((struct mtl_buffer *)buf)->length == 256);
    CHECK(((struct mtl_buffer *)buf)->options == 0);
    CHECK(metaldev_new_buffer(dev, 0, 0) == NULL);
    CHECK(metaldev_new_buffer(dev, MTLDEV_MAX_BUFFER_BYTES + 1, 0) == NULL);
    CHECK(metaldev_new_buffer(NULL, 64, 0) == NULL);
    /* The memfd value 42 is never touched, only stored. */
    tex = metaldev_new_texture_iosurface(dev, 42, 1080, 2400, 80);
    CHECK(tex != NULL);
    CHECK(((struct mtl_texture *)tex)->memfd == 42);
    CHECK(((struct mtl_texture *)tex)->width == 1080);
    CHECK(((struct mtl_texture *)tex)->height == 2400);
    CHECK(((struct mtl_texture *)tex)->pixel_format == 80);
    CHECK(metaldev_new_texture_iosurface(dev, -1, 1080, 2400, 80) == NULL);
    CHECK(metaldev_new_texture_iosurface(dev, 42, 0, 2400, 80) == NULL);
    CHECK(metaldev_new_texture_iosurface(NULL, 42, 1080, 2400, 80) == NULL);
    pipe = metaldev_new_render_pipeline(dev, "vertex_passthrough", "fragment_passthrough");
    CHECK(pipe != NULL);
    CHECK(strcmp(((struct mtl_pipeline *)pipe)->vertex_fn, "vertex_passthrough") == 0);
    CHECK(strcmp(((struct mtl_pipeline *)pipe)->fragment_fn, "fragment_passthrough") == 0);
    CHECK(((struct mtl_pipeline *)pipe)->passthrough == 1);
    CHECK(metaldev_new_render_pipeline(dev, NULL, "fragment_passthrough") == NULL);
    CHECK(metaldev_new_render_pipeline(dev, "vertex_passthrough", NULL) == NULL);
    CHECK(metaldev_new_command_buffer(NULL) == NULL);

    /* ObjC dispatch path: call IMPs retrieved from the mock registry. */
    {
        typedef void *(*imp_new_queue_fn)(void *, const char *);
        typedef void *(*imp_cmdbuf_fn)(void *, const char *);
        imp_new_queue_fn imp_q;
        imp_cmdbuf_fn imp_cb;
        imp_q = (imp_new_queue_fn)mock_lookup_method(cls_device, "newCommandQueue");
        CHECK(imp_q != NULL);
        q2 = (mtl_queue_t)imp_q(dev, "newCommandQueue");
        CHECK(q2 != NULL);
        imp_cb = (imp_cmdbuf_fn)mock_lookup_method(cls_queue, "commandBuffer");
        CHECK(imp_cb != NULL);
        cb = (mtl_cmdbuf_t)imp_cb(q2, "commandBuffer");
        CHECK(cb != NULL);
    }

    /* Buffer and texture getters through their IMPs. */
    {
        typedef unsigned long (*imp_ulong_fn)(void *, const char *);
        typedef void *(*imp_ptr_fn)(void *, const char *);
        imp_ulong_fn imp_len;
        imp_ptr_fn imp_contents;
        imp_ulong_fn imp_w;
        imp_ulong_fn imp_h;
        imp_ulong_fn imp_fmt;
        imp_len = (imp_ulong_fn)mock_lookup_method(cls_buffer, "length");
        imp_contents = (imp_ptr_fn)mock_lookup_method(cls_buffer, "contents");
        CHECK(imp_len != NULL && imp_len(buf, "length") == 256);
        CHECK(imp_contents != NULL &&
              imp_contents(buf, "contents") ==
                  (void *)((struct mtl_buffer *)buf)->storage);
        imp_w = (imp_ulong_fn)mock_lookup_method(cls_texture, "width");
        imp_h = (imp_ulong_fn)mock_lookup_method(cls_texture, "height");
        imp_fmt = (imp_ulong_fn)mock_lookup_method(cls_texture, "pixelFormat");
        CHECK(imp_w != NULL && imp_w(tex, "width") == 1080);
        CHECK(imp_h != NULL && imp_h(tex, "height") == 2400);
        CHECK(imp_fmt != NULL && imp_fmt(tex, "pixelFormat") == 80);
    }

    /* Pipeline getters through their IMPs. */
    {
        typedef const char *(*imp_str_fn)(void *, const char *);
        imp_str_fn imp_v;
        imp_str_fn imp_f;
        imp_v = (imp_str_fn)mock_lookup_method(cls_pipeline, "vertexFunction");
        imp_f = (imp_str_fn)mock_lookup_method(cls_pipeline, "fragmentFunction");
        CHECK(imp_v != NULL &&
              strcmp(imp_v(pipe, "vertexFunction"), "vertex_passthrough") == 0);
        CHECK(imp_f != NULL &&
              strcmp(imp_f(pipe, "fragmentFunction"), "fragment_passthrough") == 0);
    }

    /* Render encoder flow through its IMPs: set pipeline, set clear
     * color, draw, end. The draw is recorded in the command buffer. */
    cb_s = (struct mtl_cmdbuf *)cb;
    CHECK(cb_s->count == 0);
    {
        typedef void *(*imp_new_encoder_fn)(void *, const char *, void *);
        typedef void (*imp_set_pipeline_fn)(void *, const char *, void *);
        typedef void (*imp_set_clear_fn)(void *, const char *, float, float, float, float);
        typedef void (*imp_draw_fn)(void *, const char *, unsigned long, unsigned long, unsigned long);
        typedef void (*imp_void_fn)(void *, const char *);
        imp_new_encoder_fn imp_new_enc;
        imp_set_pipeline_fn imp_set;
        imp_set_clear_fn imp_cc;
        imp_draw_fn imp_draw;
        imp_void_fn imp_end;
        void *enc;
        imp_new_enc = (imp_new_encoder_fn)mock_lookup_method(cls_cmdbuf, "renderCommandEncoderWithDescriptor:");
        CHECK(imp_new_enc != NULL);
        enc = imp_new_enc(cb, "renderCommandEncoderWithDescriptor:", NULL);
        CHECK(enc != NULL);
        imp_set = (imp_set_pipeline_fn)mock_lookup_method(cls_encoder, "setRenderPipelineState:");
        CHECK(imp_set != NULL);
        imp_set(enc, "setRenderPipelineState:", pipe);
        CHECK(((struct mtl_encoder *)enc)->pipeline == pipe);
        imp_cc = (imp_set_clear_fn)mock_lookup_method(cls_encoder, "setClearColor:");
        CHECK(imp_cc != NULL);
        imp_cc(enc, "setClearColor:", 0.1f, 0.2f, 0.3f, 1.0f);
        CHECK(((struct mtl_encoder *)enc)->has_clear_color == 1);
        CHECK(((struct mtl_encoder *)enc)->r == 0.1f);
        imp_draw = (imp_draw_fn)mock_lookup_method(cls_encoder, "drawPrimitives:vertexStart:vertexCount:");
        CHECK(imp_draw != NULL);
        imp_draw(enc, "drawPrimitives:vertexStart:vertexCount:", 4, 0, 3);
        CHECK(cb_s->count == 1);
        CHECK(cb_s->cmds[0].kind == MTL_CMD_DRAW);
        CHECK(cb_s->cmds[0].pipeline == pipe);
        CHECK(cb_s->cmds[0].primitive == 4);
        CHECK(cb_s->cmds[0].vertex_start == 0);
        CHECK(cb_s->cmds[0].vertex_count == 3);
        imp_end = (imp_void_fn)mock_lookup_method(cls_encoder, "endEncoding");
        CHECK(imp_end != NULL);
        imp_end(enc, "endEncoding");
        CHECK(((struct mtl_encoder *)enc)->ended == 1);
        /* A draw after endEncoding is ignored. */
        imp_draw(enc, "drawPrimitives:vertexStart:vertexCount:", 4, 0, 3);
        CHECK(cb_s->count == 1);
    }

    /* Clear-color encoding through the C entry point. */
    CHECK(metaldev_clear_color(cb, tex, 0.1f, 0.2f, 0.3f, 1.0f) == 0);
    CHECK(cb_s->count == 2);
    CHECK(cb_s->cmds[1].kind == MTL_CMD_CLEAR_COLOR);
    CHECK(cb_s->cmds[1].texture == tex);
    CHECK(cb_s->cmds[1].r == 0.1f);
    CHECK(cb_s->cmds[1].g == 0.2f);
    CHECK(cb_s->cmds[1].b == 0.3f);
    CHECK(cb_s->cmds[1].a == 1.0f);
    CHECK(metaldev_clear_color(NULL, tex, 0.0f, 0.0f, 0.0f, 0.0f) == -1);
    CHECK(metaldev_clear_color(cb, NULL, 0.0f, 0.0f, 0.0f, 0.0f) == -1);

    /* Commit with no Vulkan shim attached: records and succeeds. */
    CHECK(metaldev_commit(cb) == 0);
    CHECK(cb_s->committed == 1);
    CHECK(cb_s->submitted_via_shim == 0);
    /* Commit is idempotent. */
    CHECK(metaldev_commit(cb) == 0);
    CHECK(metaldev_commit(NULL) == -1);
    /* Commit through the IMP as well. */
    {
        typedef void (*imp_commit_fn)(void *, const char *);
        imp_commit_fn imp_c;
        imp_c = (imp_commit_fn)mock_lookup_method(cls_cmdbuf, "commit");
        CHECK(imp_c != NULL);
        imp_c(cb, "commit");
        CHECK(cb_s->committed == 1);
    }
    /* No encoding after commit. */
    CHECK(metaldev_clear_color(cb, tex, 0.0f, 0.0f, 0.0f, 0.0f) == -1);

    /* Commit with a stub shim attached: submission is recorded. */
    dev_s->vk = &shim;
    cb2 = metaldev_new_command_buffer(q);
    CHECK(cb2 != NULL);
    CHECK(metaldev_clear_color(cb2, tex, 0.0f, 0.0f, 0.0f, 1.0f) == 0);
    CHECK(metaldev_commit(cb2) == 0);
    CHECK(((struct mtl_cmdbuf *)cb2)->submitted_via_shim == 1);
    CHECK(((struct mtl_cmdbuf *)cb2)->committed == 1);

    /* Commit with a broken shim (submit entries missing) fails. */
    dev_s->vk = &bad_shim;
    cb3 = metaldev_new_command_buffer(q);
    CHECK(cb3 != NULL);
    CHECK(metaldev_clear_color(cb3, tex, 0.0f, 0.0f, 0.0f, 1.0f) == 0);
    CHECK(metaldev_commit(cb3) == -1);
    CHECK(((struct mtl_cmdbuf *)cb3)->committed == 0);
    dev_s->vk = NULL;

    printf("metaldev_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
