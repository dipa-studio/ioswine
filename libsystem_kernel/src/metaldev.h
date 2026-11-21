/*
 * metaldev.h: our own MTLDevice on Vulkan for ioswine.
 *
 * Registers seven fake Metal classes with the ObjC runtime and exposes
 * C entry points that the registered ObjC methods call. The classes are
 * IOSwineMTLDevice, IOSwineMTLCommandQueue, IOSwineMTLCommandBuffer,
 * IOSwineMTLRenderCommandEncoder, IOSwineMTLBuffer, IOSwineMTLTexture
 * and IOSwineMTLRenderPipelineState. All objects are plain C structs
 * whose first field is the isa pointer; dispatch is by if/switch, never
 * by static function-pointer tables (see ARCHITECTURE.md section 6).
 *
 * Style: freestanding C, no libc. Objects come from fixed-size static
 * pools in metaldev.c; there is no release path in this skeleton, so a
 * pool that runs out makes the corresponding new_* call return NULL.
 * Staged progression: device discovered -> clear color frame -> CA compositing
 * shaders -> RenderBox (SwiftUI) -> Liquid Glass effects.
 *
 * The Vulkan side lives in vkshim.h. Until the AIR->SPIR-V translator
 * lands, pipelines use a passthrough shader stub: the
 * function names are recorded, no shader code is compiled.
 *
 * Method names registered on each class (selectors are ours; they
 * mirror the real Metal selectors where the argument shape matches,
 * and say what they take where it does not, e.g. the texture
 * constructor takes a memfd, not an IOSurfaceRef):
 *   IOSwineMTLDevice:
 *     newCommandQueue
 *     newBufferWithLength:options:
 *     newTextureWithMemfd:width:height:pixelFormat:
 *     newRenderPipelineStateWithVertexFunction:fragmentFunction:
 *   IOSwineMTLCommandQueue:
 *     commandBuffer
 *   IOSwineMTLCommandBuffer:
 *     commit
 *     renderCommandEncoderWithDescriptor:
 *   IOSwineMTLRenderCommandEncoder:
 *     setRenderPipelineState:
 *     setClearColor:
 *     drawPrimitives:vertexStart:vertexCount:
 *     endEncoding
 *   IOSwineMTLBuffer:
 *     contents
 *     length
 *   IOSwineMTLTexture:
 *     width
 *     height
 *     pixelFormat
 *   IOSwineMTLRenderPipelineState:
 *     vertexFunction
 *     fragmentFunction
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

struct vk_shim; /* vkshim.h; only a pointer is needed here. */

typedef void *mtl_device_t;
typedef void *mtl_queue_t;
typedef void *mtl_buffer_t;
typedef void *mtl_texture_t;
typedef void *mtl_cmdbuf_t;
typedef void *mtl_pipeline_t;

/* ObjC runtime indirection: filled at runtime, never statically. */
struct mtl_objc_runtime {
    void *(*allocate_class_pair)(void *superclass, const char *name, unsigned long extra_bytes);
    int   (*class_add_method)(void *cls, const char *sel, void *imp, const char *types);
    void  (*register_class_pair)(void *cls);
    void *(*get_class)(const char *name);
};

/* Pool limits. Buffers larger than MTLDEV_MAX_BUFFER_BYTES fail in
 * this skeleton; a real allocator lands with the GPU helper. */
#define MTLDEV_MAX_DEVICES       4
#define MTLDEV_MAX_QUEUES        16
#define MTLDEV_MAX_BUFFERS       16
#define MTLDEV_MAX_BUFFER_BYTES  4096
#define MTLDEV_MAX_TEXTURES      16
#define MTLDEV_MAX_CMDBUFS       8
#define MTLDEV_MAX_COMMANDS      16
#define MTLDEV_MAX_PIPELINES     8
#define MTLDEV_MAX_ENCODERS      8
#define MTLDEV_MAX_FN_NAME       128

/* One recorded command in a command buffer. metaldev_clear_color()
 * appends MTL_CMD_CLEAR_COLOR; the render encoder's draw method
 * appends MTL_CMD_DRAW. The Vulkan submit path (once it exists) reads
 * this list; until then it is the encoding the host test verifies. */
enum mtl_cmd_kind {
    MTL_CMD_CLEAR_COLOR = 1,
    MTL_CMD_DRAW = 2
};

struct mtl_cmd {
    unsigned int kind;          /* enum mtl_cmd_kind */
    mtl_texture_t texture;      /* MTL_CMD_CLEAR_COLOR: render target */
    float r, g, b, a;           /* MTL_CMD_CLEAR_COLOR: clear color */
    mtl_pipeline_t pipeline;    /* MTL_CMD_DRAW: pipeline to bind */
    unsigned int primitive;     /* MTL_CMD_DRAW: primitive type, raw value */
    unsigned long vertex_start; /* MTL_CMD_DRAW */
    unsigned long vertex_count; /* MTL_CMD_DRAW */
};

/* Object layouts. The first field is always the isa pointer. The
 * in_use field is pool bookkeeping, not a Metal field. */

struct mtl_device {
    void *isa;              /* class IOSwineMTLDevice */
    struct vk_shim *vk;     /* runtime-filled Vulkan shim, or NULL.
                               Set by the GPU-helper integration code (or
                               the host test) by direct assignment; there
                               is no setter in this skeleton. When NULL,
                               metaldev_commit() records the commands and
                               returns success without submitting. */
    unsigned int in_use;
    unsigned int device_index; /* 0 for the default device */
};

struct mtl_queue {
    void *isa;              /* class IOSwineMTLCommandQueue */
    unsigned int in_use;
    struct mtl_device *device;
};

struct mtl_buffer {
    void *isa;              /* class IOSwineMTLBuffer */
    unsigned int in_use;
    struct mtl_device *device;
    unsigned long length;
    unsigned int options;
    unsigned char storage[MTLDEV_MAX_BUFFER_BYTES]; /* contents() returns this */
};

struct mtl_texture {
    void *isa;              /* class IOSwineMTLTexture */
    unsigned int in_use;
    struct mtl_device *device;
    int memfd;              /* memfd-backed IOSurface interface;
                               stored, never touched: no mmap, no close. */
    int width;
    int height;
    unsigned int pixel_format; /* raw MTLPixelFormat value, not validated */
};

struct mtl_cmdbuf {
    void *isa;              /* class IOSwineMTLCommandBuffer */
    unsigned int in_use;
    struct mtl_queue *queue;
    struct mtl_device *device;
    unsigned int count;     /* recorded commands in cmds[] */
    struct mtl_cmd cmds[MTLDEV_MAX_COMMANDS];
    unsigned int committed;        /* set by metaldev_commit() */
    unsigned int submitted_via_shim; /* set when a shim was attached at commit */
};

struct mtl_pipeline {
    void *isa;              /* class IOSwineMTLRenderPipelineState */
    unsigned int in_use;
    struct mtl_device *device;
    char vertex_fn[MTLDEV_MAX_FN_NAME];
    char fragment_fn[MTLDEV_MAX_FN_NAME];
    unsigned int passthrough; /* 1: passthrough shader stub */
};

struct mtl_encoder {
    void *isa;              /* class IOSwineMTLRenderCommandEncoder */
    unsigned int in_use;
    struct mtl_cmdbuf *cmdbuf;
    mtl_pipeline_t pipeline; /* set by setRenderPipelineState: */
    float r, g, b, a;        /* set by setClearColor: */
    unsigned int has_clear_color;
    unsigned int ended;     /* set by endEncoding */
};

/* Register the classes above with the given ObjC runtime and create the
   default device. Returns 0 on success, -1 on failure. Idempotent. */
int metaldev_init_with_runtime(const struct mtl_objc_runtime *rt);

/* Same, using the real runtime resolved by name at runtime. In this
   skeleton the real lookup is a stub that fails gracefully (returns -1)
   with a note: the real ObjC lookup lands with the ObjC runtime
   integration. Do not fake success. */
int metaldev_init(void);

/* The default device created by metaldev_init*(), or NULL. */
mtl_device_t metaldev_default_device(void);

/* C entry points; the ObjC methods registered above call these. */
mtl_queue_t    metaldev_new_command_queue(mtl_device_t dev);
mtl_buffer_t   metaldev_new_buffer(mtl_device_t dev, unsigned long length, unsigned int options);
mtl_texture_t  metaldev_new_texture_iosurface(mtl_device_t dev, int memfd, int width, int height, unsigned int pixel_format);
mtl_cmdbuf_t   metaldev_new_command_buffer(mtl_queue_t q);
mtl_pipeline_t metaldev_new_render_pipeline(mtl_device_t dev, const char *vertex_fn, const char *fragment_fn);

/* Encode a clear-color render pass into a command buffer and commit it.
   Uses the passthrough shader stub until the AIR->SPIR-V translator
 lands. Returns 0 on success. */
int metaldev_clear_color(mtl_cmdbuf_t cb, mtl_texture_t tex, float r, float g, float b, float a);
int metaldev_commit(mtl_cmdbuf_t cb);
