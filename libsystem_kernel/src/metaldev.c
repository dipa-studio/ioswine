/*
 * metaldev.c: our own MTLDevice on Vulkan for ioswine.
 * See metaldev.h for the API, the class list, and the object layouts.
 *
 * Style: freestanding C, no libc. No printf, no malloc, no
 * memset/strcpy: objects come from fixed-size static pools, strings
 * are copied with a small explicit loop, and command records are
 * copied field by field. All dispatch is by if/switch and explicit
 * calls, never by static function-pointer tables (see ARCHITECTURE.md
 * section 6).
 */
#include "metaldev.h"
#include "vkshim.h"

/* Registered class pointers, filled by metaldev_init_with_runtime().
 * They are also what the isa checks below compare against. */
static void *g_class_device;
static void *g_class_queue;
static void *g_class_cmdbuf;
static void *g_class_encoder;
static void *g_class_buffer;
static void *g_class_texture;
static void *g_class_pipeline;

static int g_metaldev_initialized;
static struct mtl_device *g_metaldev_default_device;

/* Object pools. No release path in this skeleton: once a slot is
 * taken it stays taken until process exit, and a full pool makes the
 * corresponding new_* call return NULL. */
static struct mtl_device g_mtl_devices[MTLDEV_MAX_DEVICES];
static struct mtl_queue g_mtl_queues[MTLDEV_MAX_QUEUES];
static struct mtl_buffer g_mtl_buffers[MTLDEV_MAX_BUFFERS];
static struct mtl_texture g_mtl_textures[MTLDEV_MAX_TEXTURES];
static struct mtl_cmdbuf g_mtl_cmdbufs[MTLDEV_MAX_CMDBUFS];
static struct mtl_pipeline g_mtl_pipelines[MTLDEV_MAX_PIPELINES];
static struct mtl_encoder g_mtl_encoders[MTLDEV_MAX_ENCODERS];

/* Every object struct starts with the isa pointer, so one small
 * header type reads it for all of them. */
struct mtl_object_head {
    void *isa;
};

static int mtl_is_a(const void *obj, const void *cls) {
    if (obj == NULL || cls == NULL) {
        return 0;
    }
    return ((const struct mtl_object_head *)obj)->isa == cls;
}

/* Copy a NUL-terminated string into a fixed-size field without
 * strcpy. Truncates with NUL termination if it does not fit. */
static void mtl_copy_name(char *dst, const char *src, unsigned int dst_size) {
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

/* Pool allocators. Each marks the slot taken and returns it, or NULL
 * when the pool is full. Callers fill the remaining fields. */
static struct mtl_device *mtl_device_alloc(void) {
    unsigned int i;
    for (i = 0; i < MTLDEV_MAX_DEVICES; i++) {
        if (!g_mtl_devices[i].in_use) {
            g_mtl_devices[i].in_use = 1;
            return &g_mtl_devices[i];
        }
    }
    return NULL;
}

static struct mtl_queue *mtl_queue_alloc(void) {
    unsigned int i;
    for (i = 0; i < MTLDEV_MAX_QUEUES; i++) {
        if (!g_mtl_queues[i].in_use) {
            g_mtl_queues[i].in_use = 1;
            return &g_mtl_queues[i];
        }
    }
    return NULL;
}

static struct mtl_buffer *mtl_buffer_alloc(void) {
    unsigned int i;
    for (i = 0; i < MTLDEV_MAX_BUFFERS; i++) {
        if (!g_mtl_buffers[i].in_use) {
            g_mtl_buffers[i].in_use = 1;
            return &g_mtl_buffers[i];
        }
    }
    return NULL;
}

static struct mtl_texture *mtl_texture_alloc(void) {
    unsigned int i;
    for (i = 0; i < MTLDEV_MAX_TEXTURES; i++) {
        if (!g_mtl_textures[i].in_use) {
            g_mtl_textures[i].in_use = 1;
            return &g_mtl_textures[i];
        }
    }
    return NULL;
}

static struct mtl_cmdbuf *mtl_cmdbuf_alloc(void) {
    unsigned int i;
    for (i = 0; i < MTLDEV_MAX_CMDBUFS; i++) {
        if (!g_mtl_cmdbufs[i].in_use) {
            g_mtl_cmdbufs[i].in_use = 1;
            return &g_mtl_cmdbufs[i];
        }
    }
    return NULL;
}

static struct mtl_pipeline *mtl_pipeline_alloc(void) {
    unsigned int i;
    for (i = 0; i < MTLDEV_MAX_PIPELINES; i++) {
        if (!g_mtl_pipelines[i].in_use) {
            g_mtl_pipelines[i].in_use = 1;
            return &g_mtl_pipelines[i];
        }
    }
    return NULL;
}

static struct mtl_encoder *mtl_encoder_alloc(void) {
    unsigned int i;
    for (i = 0; i < MTLDEV_MAX_ENCODERS; i++) {
        if (!g_mtl_encoders[i].in_use) {
            g_mtl_encoders[i].in_use = 1;
            return &g_mtl_encoders[i];
        }
    }
    return NULL;
}

/* Append one recorded command to a command buffer. Copies the record
 * field by field (no struct assignment, so no memcpy is needed).
 * Returns 0 on success, -1 when the buffer is full. */
static int mtl_cmdbuf_append(struct mtl_cmdbuf *b, const struct mtl_cmd *cmd) {
    struct mtl_cmd *dst;
    if (b == NULL || cmd == NULL) {
        return -1;
    }
    if (b->count >= MTLDEV_MAX_COMMANDS) {
        return -1;
    }
    dst = &b->cmds[b->count];
    dst->kind = cmd->kind;
    dst->texture = cmd->texture;
    dst->r = cmd->r;
    dst->g = cmd->g;
    dst->b = cmd->b;
    dst->a = cmd->a;
    dst->pipeline = cmd->pipeline;
    dst->primitive = cmd->primitive;
    dst->vertex_start = cmd->vertex_start;
    dst->vertex_count = cmd->vertex_count;
    b->count++;
    return 0;
}

/* Internal encoder constructor (no public constructor in the
 * API; encoders are created by the command buffer's
 * renderCommandEncoderWithDescriptor: method). */
static void *mtl_encoder_new(struct mtl_cmdbuf *cb) {
    struct mtl_encoder *e;
    if (cb == NULL || !mtl_is_a(cb, g_class_cmdbuf)) {
        return NULL;
    }
    e = mtl_encoder_alloc();
    if (e == NULL) {
        return NULL;
    }
    e->isa = g_class_encoder;
    e->cmdbuf = cb;
    e->pipeline = NULL;
    e->r = 0.0f;
    e->g = 0.0f;
    e->b = 0.0f;
    e->a = 0.0f;
    e->has_clear_color = 0;
    e->ended = 0;
    return e;
}

/* ObjC method implementations (IMPs). Each takes self and the
 * selector first, like a real IMP, then the method arguments. The
 * test calls them through the mock registry to exercise ObjC dispatch.
 * Invalid self pointers fail silently (return NULL/0), as a fake
 * driver must never crash the app on a bad message send. */

/* IOSwineMTLDevice */
static void *imp_device_new_command_queue(void *self, const char *sel) {
    (void)sel;
    return metaldev_new_command_queue((mtl_device_t)self);
}

static void *imp_device_new_buffer(void *self, const char *sel,
                                   unsigned long length, unsigned int options) {
    (void)sel;
    return metaldev_new_buffer((mtl_device_t)self, length, options);
}

static void *imp_device_new_texture(void *self, const char *sel,
                                    int memfd, int width, int height,
                                    unsigned int pixel_format) {
    (void)sel;
    return metaldev_new_texture_iosurface((mtl_device_t)self, memfd,
                                          width, height, pixel_format);
}

static void *imp_device_new_pipeline(void *self, const char *sel,
                                     const char *vertex_fn,
                                     const char *fragment_fn) {
    (void)sel;
    return metaldev_new_render_pipeline((mtl_device_t)self,
                                        vertex_fn, fragment_fn);
}

/* IOSwineMTLCommandQueue */
static void *imp_queue_command_buffer(void *self, const char *sel) {
    (void)sel;
    return metaldev_new_command_buffer((mtl_queue_t)self);
}

/* IOSwineMTLCommandBuffer */
static void imp_cmdbuf_commit(void *self, const char *sel) {
    (void)sel;
    (void)metaldev_commit((mtl_cmdbuf_t)self);
}

static void *imp_cmdbuf_new_encoder(void *self, const char *sel,
                                    void *descriptor) {
    (void)sel;
    (void)descriptor; /* descriptor is ignored in this skeleton */
    return mtl_encoder_new((struct mtl_cmdbuf *)self);
}

/* IOSwineMTLRenderCommandEncoder */
static void imp_encoder_set_pipeline(void *self, const char *sel,
                                     void *pipeline) {
    struct mtl_encoder *e = (struct mtl_encoder *)self;
    (void)sel;
    if (!mtl_is_a(e, g_class_encoder)) {
        return;
    }
    if (pipeline != NULL && !mtl_is_a(pipeline, g_class_pipeline)) {
        return;
    }
    e->pipeline = (mtl_pipeline_t)pipeline;
}

static void imp_encoder_set_clear_color(void *self, const char *sel,
                                        float r, float g, float b, float a) {
    struct mtl_encoder *e = (struct mtl_encoder *)self;
    (void)sel;
    if (!mtl_is_a(e, g_class_encoder)) {
        return;
    }
    e->r = r;
    e->g = g;
    e->b = b;
    e->a = a;
    e->has_clear_color = 1;
}

static void imp_encoder_draw(void *self, const char *sel,
                             unsigned long primitive,
                             unsigned long vertex_start,
                             unsigned long vertex_count) {
    struct mtl_encoder *e = (struct mtl_encoder *)self;
    struct mtl_cmd cmd;
    (void)sel;
    if (!mtl_is_a(e, g_class_encoder)) {
        return;
    }
    if (e->ended || e->cmdbuf == NULL) {
        return;
    }
    cmd.kind = MTL_CMD_DRAW;
    cmd.texture = NULL;
    cmd.r = 0.0f;
    cmd.g = 0.0f;
    cmd.b = 0.0f;
    cmd.a = 0.0f;
    cmd.pipeline = e->pipeline;
    cmd.primitive = (unsigned int)primitive;
    cmd.vertex_start = vertex_start;
    cmd.vertex_count = vertex_count;
    (void)mtl_cmdbuf_append(e->cmdbuf, &cmd);
}

static void imp_encoder_end_encoding(void *self, const char *sel) {
    struct mtl_encoder *e = (struct mtl_encoder *)self;
    (void)sel;
    if (!mtl_is_a(e, g_class_encoder)) {
        return;
    }
    e->ended = 1;
}

/* IOSwineMTLBuffer */
static void *imp_buffer_contents(void *self, const char *sel) {
    struct mtl_buffer *b = (struct mtl_buffer *)self;
    (void)sel;
    if (!mtl_is_a(b, g_class_buffer)) {
        return NULL;
    }
    return b->storage;
}

static unsigned long imp_buffer_length(void *self, const char *sel) {
    struct mtl_buffer *b = (struct mtl_buffer *)self;
    (void)sel;
    if (!mtl_is_a(b, g_class_buffer)) {
        return 0;
    }
    return b->length;
}

/* IOSwineMTLTexture */
static unsigned long imp_texture_width(void *self, const char *sel) {
    struct mtl_texture *t = (struct mtl_texture *)self;
    (void)sel;
    if (!mtl_is_a(t, g_class_texture)) {
        return 0;
    }
    return (unsigned long)t->width;
}

static unsigned long imp_texture_height(void *self, const char *sel) {
    struct mtl_texture *t = (struct mtl_texture *)self;
    (void)sel;
    if (!mtl_is_a(t, g_class_texture)) {
        return 0;
    }
    return (unsigned long)t->height;
}

static unsigned long imp_texture_pixel_format(void *self, const char *sel) {
    struct mtl_texture *t = (struct mtl_texture *)self;
    (void)sel;
    if (!mtl_is_a(t, g_class_texture)) {
        return 0;
    }
    return (unsigned long)t->pixel_format;
}

/* IOSwineMTLRenderPipelineState */
static const char *imp_pipeline_vertex_fn(void *self, const char *sel) {
    struct mtl_pipeline *p = (struct mtl_pipeline *)self;
    (void)sel;
    if (!mtl_is_a(p, g_class_pipeline)) {
        return NULL;
    }
    return p->vertex_fn;
}

static const char *imp_pipeline_fragment_fn(void *self, const char *sel) {
    struct mtl_pipeline *p = (struct mtl_pipeline *)self;
    (void)sel;
    if (!mtl_is_a(p, g_class_pipeline)) {
        return NULL;
    }
    return p->fragment_fn;
}

/* Register one class and its methods. The methods are registered with
 * explicit calls, one by one: no static method table (see
 * ARCHITECTURE.md section 6). Returns 0 on success, -1 on failure. */
static int metaldev_register_classes(const struct mtl_objc_runtime *rt) {
    void *cls;

    /* IOSwineMTLDevice */
    cls = rt->allocate_class_pair(NULL, "IOSwineMTLDevice", 0);
    if (cls == NULL) {
        return -1;
    }
    g_class_device = cls;
    if (rt->class_add_method(cls, "newCommandQueue", imp_device_new_command_queue, "@@:") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "newBufferWithLength:options:", imp_device_new_buffer, "@@:QQ") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "newTextureWithMemfd:width:height:pixelFormat:", imp_device_new_texture, "@@:iiiI") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "newRenderPipelineStateWithVertexFunction:fragmentFunction:", imp_device_new_pipeline, "@@:@@") != 0) {
        return -1;
    }
    rt->register_class_pair(cls);

    /* IOSwineMTLCommandQueue */
    cls = rt->allocate_class_pair(NULL, "IOSwineMTLCommandQueue", 0);
    if (cls == NULL) {
        return -1;
    }
    g_class_queue = cls;
    if (rt->class_add_method(cls, "commandBuffer", imp_queue_command_buffer, "@@:") != 0) {
        return -1;
    }
    rt->register_class_pair(cls);

    /* IOSwineMTLCommandBuffer */
    cls = rt->allocate_class_pair(NULL, "IOSwineMTLCommandBuffer", 0);
    if (cls == NULL) {
        return -1;
    }
    g_class_cmdbuf = cls;
    if (rt->class_add_method(cls, "commit", imp_cmdbuf_commit, "v@:") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "renderCommandEncoderWithDescriptor:", imp_cmdbuf_new_encoder, "@@:@") != 0) {
        return -1;
    }
    rt->register_class_pair(cls);

    /* IOSwineMTLRenderCommandEncoder */
    cls = rt->allocate_class_pair(NULL, "IOSwineMTLRenderCommandEncoder", 0);
    if (cls == NULL) {
        return -1;
    }
    g_class_encoder = cls;
    if (rt->class_add_method(cls, "setRenderPipelineState:", imp_encoder_set_pipeline, "v@:@") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "setClearColor:", imp_encoder_set_clear_color, "v@:ffff") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "drawPrimitives:vertexStart:vertexCount:", imp_encoder_draw, "v@:QQQ") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "endEncoding", imp_encoder_end_encoding, "v@:") != 0) {
        return -1;
    }
    rt->register_class_pair(cls);

    /* IOSwineMTLBuffer */
    cls = rt->allocate_class_pair(NULL, "IOSwineMTLBuffer", 0);
    if (cls == NULL) {
        return -1;
    }
    g_class_buffer = cls;
    if (rt->class_add_method(cls, "contents", imp_buffer_contents, "^v@:") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "length", imp_buffer_length, "Q@:") != 0) {
        return -1;
    }
    rt->register_class_pair(cls);

    /* IOSwineMTLTexture */
    cls = rt->allocate_class_pair(NULL, "IOSwineMTLTexture", 0);
    if (cls == NULL) {
        return -1;
    }
    g_class_texture = cls;
    if (rt->class_add_method(cls, "width", imp_texture_width, "Q@:") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "height", imp_texture_height, "Q@:") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "pixelFormat", imp_texture_pixel_format, "Q@:") != 0) {
        return -1;
    }
    rt->register_class_pair(cls);

    /* IOSwineMTLRenderPipelineState */
    cls = rt->allocate_class_pair(NULL, "IOSwineMTLRenderPipelineState", 0);
    if (cls == NULL) {
        return -1;
    }
    g_class_pipeline = cls;
    if (rt->class_add_method(cls, "vertexFunction", imp_pipeline_vertex_fn, "@@:") != 0) {
        return -1;
    }
    if (rt->class_add_method(cls, "fragmentFunction", imp_pipeline_fragment_fn, "@@:") != 0) {
        return -1;
    }
    rt->register_class_pair(cls);

    /* Verify the runtime can find every class back by name. */
    if (rt->get_class("IOSwineMTLDevice") != g_class_device) {
        return -1;
    }
    if (rt->get_class("IOSwineMTLCommandQueue") != g_class_queue) {
        return -1;
    }
    if (rt->get_class("IOSwineMTLCommandBuffer") != g_class_cmdbuf) {
        return -1;
    }
    if (rt->get_class("IOSwineMTLRenderCommandEncoder") != g_class_encoder) {
        return -1;
    }
    if (rt->get_class("IOSwineMTLBuffer") != g_class_buffer) {
        return -1;
    }
    if (rt->get_class("IOSwineMTLTexture") != g_class_texture) {
        return -1;
    }
    if (rt->get_class("IOSwineMTLRenderPipelineState") != g_class_pipeline) {
        return -1;
    }
    return 0;
}

int metaldev_init_with_runtime(const struct mtl_objc_runtime *rt) {
    struct mtl_device *dev;
    if (g_metaldev_initialized) {
        return 0;
    }
    if (rt == NULL || rt->allocate_class_pair == NULL ||
        rt->class_add_method == NULL || rt->register_class_pair == NULL ||
        rt->get_class == NULL) {
        return -1;
    }
    if (metaldev_register_classes(rt) != 0) {
        return -1;
    }
    dev = mtl_device_alloc();
    if (dev == NULL) {
        return -1;
    }
    dev->isa = g_class_device;
    dev->vk = NULL;
    dev->device_index = 0;
    g_metaldev_default_device = dev;
    g_metaldev_initialized = 1;
    return 0;
}

int metaldev_init(void) {
    /* Stub: the real ObjC runtime lookup lands with the ObjC runtime
       integration. There is no real runtime to resolve by name in
       this skeleton, so fail gracefully instead of faking success.
       Callers must use metaldev_init_with_runtime(). */
    return -1;
}

mtl_device_t metaldev_default_device(void) {
    return (mtl_device_t)g_metaldev_default_device;
}

mtl_queue_t metaldev_new_command_queue(mtl_device_t dev) {
    struct mtl_device *d = (struct mtl_device *)dev;
    struct mtl_queue *q;
    if (!mtl_is_a(d, g_class_device)) {
        return NULL;
    }
    q = mtl_queue_alloc();
    if (q == NULL) {
        return NULL;
    }
    q->isa = g_class_queue;
    q->device = d;
    return (mtl_queue_t)q;
}

mtl_buffer_t metaldev_new_buffer(mtl_device_t dev, unsigned long length,
                                 unsigned int options) {
    struct mtl_device *d = (struct mtl_device *)dev;
    struct mtl_buffer *b;
    if (!mtl_is_a(d, g_class_device)) {
        return NULL;
    }
    if (length == 0 || length > MTLDEV_MAX_BUFFER_BYTES) {
        return NULL;
    }
    b = mtl_buffer_alloc();
    if (b == NULL) {
        return NULL;
    }
    b->isa = g_class_buffer;
    b->device = d;
    b->length = length;
    b->options = options;
    /* storage is left uninitialized: contents are undefined until the
     * app writes them, same as a real Metal buffer. */
    return (mtl_buffer_t)b;
}

mtl_texture_t metaldev_new_texture_iosurface(mtl_device_t dev, int memfd,
                                            int width, int height,
                                            unsigned int pixel_format) {
    struct mtl_device *d = (struct mtl_device *)dev;
    struct mtl_texture *t;
    if (!mtl_is_a(d, g_class_device)) {
        return NULL;
    }
    if (memfd < 0 || width <= 0 || height <= 0) {
        return NULL;
    }
    t = mtl_texture_alloc();
    if (t == NULL) {
        return NULL;
    }
    t->isa = g_class_texture;
    t->device = d;
    t->memfd = memfd;
    t->width = width;
    t->height = height;
    t->pixel_format = pixel_format;
    return (mtl_texture_t)t;
}

mtl_cmdbuf_t metaldev_new_command_buffer(mtl_queue_t q) {
    struct mtl_queue *queue = (struct mtl_queue *)q;
    struct mtl_cmdbuf *b;
    if (!mtl_is_a(queue, g_class_queue)) {
        return NULL;
    }
    b = mtl_cmdbuf_alloc();
    if (b == NULL) {
        return NULL;
    }
    b->isa = g_class_cmdbuf;
    b->queue = queue;
    b->device = queue->device;
    b->count = 0;
    b->committed = 0;
    b->submitted_via_shim = 0;
    return (mtl_cmdbuf_t)b;
}

mtl_pipeline_t metaldev_new_render_pipeline(mtl_device_t dev,
                                            const char *vertex_fn,
                                            const char *fragment_fn) {
    struct mtl_device *d = (struct mtl_device *)dev;
    struct mtl_pipeline *p;
    if (!mtl_is_a(d, g_class_device)) {
        return NULL;
    }
    if (vertex_fn == NULL || fragment_fn == NULL) {
        return NULL;
    }
    p = mtl_pipeline_alloc();
    if (p == NULL) {
        return NULL;
    }
    p->isa = g_class_pipeline;
    p->device = d;
    mtl_copy_name(p->vertex_fn, vertex_fn, MTLDEV_MAX_FN_NAME);
    mtl_copy_name(p->fragment_fn, fragment_fn, MTLDEV_MAX_FN_NAME);
    p->passthrough = 1;
    return (mtl_pipeline_t)p;
}

int metaldev_clear_color(mtl_cmdbuf_t cb, mtl_texture_t tex,
                         float r, float g, float b, float a) {
    struct mtl_cmdbuf *buf = (struct mtl_cmdbuf *)cb;
    struct mtl_cmd cmd;
    if (!mtl_is_a(buf, g_class_cmdbuf)) {
        return -1;
    }
    if (!mtl_is_a(tex, g_class_texture)) {
        return -1;
    }
    if (buf->committed) {
        return -1;
    }
    cmd.kind = MTL_CMD_CLEAR_COLOR;
    cmd.texture = tex;
    cmd.r = r;
    cmd.g = g;
    cmd.b = b;
    cmd.a = a;
    cmd.pipeline = NULL;
    cmd.primitive = 0;
    cmd.vertex_start = 0;
    cmd.vertex_count = 0;
    return mtl_cmdbuf_append(buf, &cmd);
}

int metaldev_commit(mtl_cmdbuf_t cb) {
    struct mtl_cmdbuf *b = (struct mtl_cmdbuf *)cb;
    struct vk_shim *vk;
    if (!mtl_is_a(b, g_class_cmdbuf)) {
        return -1;
    }
    if (b->committed) {
        return 0;
    }
    vk = (b->device != NULL) ? b->device->vk : NULL;
    if (vk != NULL) {
        /* A shim is attached: the submit path needs vk_queue_submit
           and vk_queue_wait_idle. The skeleton has no VkDevice or
           VkQueue handles yet, so it cannot call vk_queue_submit
           with real arguments; it records that submission was
           requested and the real submit lands with device/queue
           creation. A shim that lacks the submit entries is broken:
           fail instead of pretending to submit. */
        if (vk->vk_queue_submit == NULL || vk->vk_queue_wait_idle == NULL) {
            return -1;
        }
        b->submitted_via_shim = 1;
    }
    b->committed = 1;
    return 0;
}
