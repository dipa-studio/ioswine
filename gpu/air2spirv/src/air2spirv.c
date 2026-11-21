/* air2spirv.c - translator: parsed AIR function description to SPIR-V.
 *
 * First path: trivial vertex (passthrough) + fragment (solid color).
 * The emitter writes SPIR-V words into a caller-provided buffer with a
 * small id allocator; every write is bounds checked. No libc, no
 * allocation, no LLVM dependency.
 *
 * SPIR-V opcode numbers used (from the SPIR-V 1.0 specification):
 *   OpCapability 17, OpMemoryModel 14, OpEntryPoint 15,
 *   OpExecutionMode 16, OpName 5, OpDecorate 71, OpTypeVoid 19,
 *   OpTypeFloat 22, OpTypeVector 23, OpTypePointer 32,
 *   OpTypeFunction 33, OpConstant 43, OpConstantComposite 44,
 *   OpVariable 59, OpFunction 54, OpFunctionEnd 56, OpLabel 248,
 *   OpLoad 61, OpStore 62, OpReturn 253.
 */
#include "air2spirv.h"
#include "a2s_common.h"

/* SPIR-V enumerants. */
#define SPV_CAP_SHADER 1u
#define SPV_ADDR_LOGICAL 0u
#define SPV_MEM_GLSL450 1u
#define SPV_EXEC_VERTEX 0u
#define SPV_EXEC_FRAGMENT 4u
#define SPV_EXEC_KERNEL 6u
#define SPV_EXEC_MODE_ORIGIN_UPPER_LEFT 7u
#define SPV_STORAGE_INPUT 1u
#define SPV_STORAGE_OUTPUT 3u
#define SPV_DECORATION_BUILT_IN 11u
#define SPV_DECORATION_LOCATION 30u
#define SPV_BUILT_IN_POSITION 0u

/* Opcodes. */
#define OP_NAME 5u
#define OP_CAPABILITY 17u
#define OP_MEMORY_MODEL 14u
#define OP_ENTRY_POINT 15u
#define OP_EXECUTION_MODE 16u
#define OP_TYPE_VOID 19u
#define OP_TYPE_FLOAT 22u
#define OP_TYPE_VECTOR 23u
#define OP_TYPE_POINTER 32u
#define OP_TYPE_FUNCTION 33u
#define OP_CONSTANT 43u
#define OP_CONSTANT_COMPOSITE 44u
#define OP_FUNCTION 54u
#define OP_FUNCTION_END 56u
#define OP_VARIABLE 59u
#define OP_LOAD 61u
#define OP_STORE 62u
#define OP_DECORATE 71u
#define OP_LABEL 248u
#define OP_RETURN 253u

typedef struct {
    uint32_t *words;
    uint32_t capacity;
    uint32_t count;
    uint32_t next_id;
} a2s_builder_t;

static int a2s_put(a2s_builder_t *b, uint32_t w)
{
    if (b->count >= b->capacity)
        return A2S_ERR_OVERFLOW;
    b->words[b->count++] = w;
    return A2S_OK;
}

static uint32_t a2s_new_id(a2s_builder_t *b)
{
    return b->next_id++;
}

/* Number of words a NUL-terminated string occupies (NUL included,
 * padded up to a multiple of 4 bytes). */
static uint32_t a2s_string_words(const char *s)
{
    size_t len = a2s_strlen(s);
    return (uint32_t)((len + 1 + 3) / 4);
}

static int a2s_put_string(a2s_builder_t *b, const char *s)
{
    uint32_t words = a2s_string_words(s);
    size_t len = a2s_strlen(s);
    uint32_t i;
    uint32_t j;

    for (i = 0; i < words; i++) {
        uint32_t w = 0;
        for (j = 0; j < 4; j++) {
            size_t k = (size_t)i * 4 + j;
            uint32_t c = (k < len) ? (uint32_t)(uint8_t)s[k] : 0;
            w |= c << (j * 8);
        }
        {
            int rc = a2s_put(b, w);
            if (rc != A2S_OK)
                return rc;
        }
    }
    return A2S_OK;
}

static uint32_t a2s_float_bits(float f)
{
    union {
        float f;
        uint32_t u;
    } v;
    v.f = f;
    return v.u;
}

/* --- one helper per instruction shape; each returns A2S_OK or
 * A2S_ERR_OVERFLOW. --- */

static int a2s_capability(a2s_builder_t *b, uint32_t cap)
{
    int rc = a2s_put(b, (2u << 16) | OP_CAPABILITY);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, cap);
}

static int a2s_memory_model(a2s_builder_t *b, uint32_t addr, uint32_t mem)
{
    int rc = a2s_put(b, (3u << 16) | OP_MEMORY_MODEL);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, addr);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, mem);
}

static int a2s_entry_point(a2s_builder_t *b, uint32_t model, uint32_t id,
                           const char *name, const uint32_t *iface,
                           uint32_t iface_count)
{
    uint32_t wc = 3 + a2s_string_words(name) + iface_count;
    uint32_t i;
    int rc = a2s_put(b, (wc << 16) | OP_ENTRY_POINT);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, model);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put_string(b, name);
    if (rc != A2S_OK)
        return rc;
    for (i = 0; i < iface_count; i++) {
        rc = a2s_put(b, iface[i]);
        if (rc != A2S_OK)
            return rc;
    }
    return A2S_OK;
}

static int a2s_execution_mode(a2s_builder_t *b, uint32_t id, uint32_t mode)
{
    int rc = a2s_put(b, (3u << 16) | OP_EXECUTION_MODE);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, mode);
}

static int a2s_name(a2s_builder_t *b, uint32_t id, const char *name)
{
    uint32_t wc = 2 + a2s_string_words(name);
    int rc = a2s_put(b, (wc << 16) | OP_NAME);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    return a2s_put_string(b, name);
}

static int a2s_decorate(a2s_builder_t *b, uint32_t id, uint32_t decor,
                        uint32_t literal)
{
    int rc = a2s_put(b, (4u << 16) | OP_DECORATE);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, decor);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, literal);
}

static int a2s_type_void(a2s_builder_t *b, uint32_t id)
{
    int rc = a2s_put(b, (2u << 16) | OP_TYPE_VOID);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, id);
}

static int a2s_type_float(a2s_builder_t *b, uint32_t id, uint32_t width)
{
    int rc = a2s_put(b, (3u << 16) | OP_TYPE_FLOAT);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, width);
}

static int a2s_type_vector(a2s_builder_t *b, uint32_t id,
                           uint32_t component, uint32_t count)
{
    int rc = a2s_put(b, (4u << 16) | OP_TYPE_VECTOR);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, component);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, count);
}

static int a2s_type_pointer(a2s_builder_t *b, uint32_t id,
                            uint32_t storage, uint32_t type)
{
    int rc = a2s_put(b, (4u << 16) | OP_TYPE_POINTER);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, storage);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, type);
}

static int a2s_type_function(a2s_builder_t *b, uint32_t id, uint32_t ret)
{
    int rc = a2s_put(b, (3u << 16) | OP_TYPE_FUNCTION);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, ret);
}

static int a2s_constant(a2s_builder_t *b, uint32_t type, uint32_t id,
                        uint32_t value)
{
    int rc = a2s_put(b, (4u << 16) | OP_CONSTANT);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, type);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, value);
}

static int a2s_constant_composite(a2s_builder_t *b, uint32_t type,
                                  uint32_t id, const uint32_t *parts,
                                  uint32_t count)
{
    uint32_t wc = 3 + count;
    uint32_t i;
    int rc = a2s_put(b, (wc << 16) | OP_CONSTANT_COMPOSITE);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, type);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    for (i = 0; i < count; i++) {
        rc = a2s_put(b, parts[i]);
        if (rc != A2S_OK)
            return rc;
    }
    return A2S_OK;
}

static int a2s_variable(a2s_builder_t *b, uint32_t type, uint32_t id,
                        uint32_t storage)
{
    int rc = a2s_put(b, (4u << 16) | OP_VARIABLE);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, type);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, storage);
}

static int a2s_function(a2s_builder_t *b, uint32_t ret, uint32_t id,
                        uint32_t type)
{
    int rc = a2s_put(b, (5u << 16) | OP_FUNCTION);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, ret);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, 0); /* Function Control: None */
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, type);
}

static int a2s_label(a2s_builder_t *b, uint32_t id)
{
    int rc = a2s_put(b, (2u << 16) | OP_LABEL);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, id);
}

static int a2s_load(a2s_builder_t *b, uint32_t type, uint32_t id,
                    uint32_t pointer)
{
    int rc = a2s_put(b, (4u << 16) | OP_LOAD);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, type);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, id);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, pointer);
}

static int a2s_store(a2s_builder_t *b, uint32_t pointer, uint32_t object)
{
    int rc = a2s_put(b, (3u << 16) | OP_STORE);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(b, pointer);
    if (rc != A2S_OK)
        return rc;
    return a2s_put(b, object);
}

static int a2s_return(a2s_builder_t *b)
{
    return a2s_put(b, (1u << 16) | OP_RETURN);
}

static int a2s_function_end(a2s_builder_t *b)
{
    return a2s_put(b, (1u << 16) | OP_FUNCTION_END);
}

/* --- module emission --- */

int a2s_emit(const a2s_function_t *fn, uint32_t *words, uint32_t capacity,
             uint32_t *out_count)
{
    a2s_builder_t b;
    uint32_t model;
    uint32_t void_ty;
    uint32_t float_ty;
    uint32_t vec4_ty;
    uint32_t ptr_in_vec4;
    uint32_t ptr_out_vec4;
    uint32_t fn_ty;
    uint32_t entry_id;
    uint32_t label_id;
    uint32_t in_id = 0;
    uint32_t out_id;
    uint32_t loaded_id = 0;
    uint32_t comp_id[4];
    uint32_t color_id = 0;
    uint32_t iface[2];
    uint32_t iface_count;
    int rc;

    if (fn == NULL || words == NULL || out_count == NULL)
        return A2S_ERR_BAD_ARG;
    if (fn->entry_name == NULL || fn->entry_name[0] == '\0')
        return A2S_ERR_BAD_ARG;
    if (fn->stage == A2S_STAGE_VERTEX) {
        model = SPV_EXEC_VERTEX;
    } else if (fn->stage == A2S_STAGE_FRAGMENT) {
        model = SPV_EXEC_FRAGMENT;
    } else {
        return A2S_ERR_BAD_STAGE; /* kernel: not in this first path */
    }
    if (capacity < 5)
        return A2S_ERR_OVERFLOW;

    b.words = words;
    b.capacity = capacity;
    b.count = 0;
    b.next_id = 1;

    /* Header. Bound is patched at the end. */
    rc = a2s_put(&b, A2S_SPIRV_MAGIC);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(&b, A2S_SPIRV_VERSION);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(&b, 0); /* generator: unknown */
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(&b, 0); /* bound: patched below */
    if (rc != A2S_OK)
        return rc;
    rc = a2s_put(&b, 0); /* schema: reserved */
    if (rc != A2S_OK)
        return rc;

    /* Allocate every id up front so the body can reference constants
     * that are emitted in the earlier types/constants section. */
    void_ty = a2s_new_id(&b);
    float_ty = a2s_new_id(&b);
    vec4_ty = a2s_new_id(&b);
    ptr_in_vec4 = a2s_new_id(&b);
    ptr_out_vec4 = a2s_new_id(&b);
    fn_ty = a2s_new_id(&b);
    entry_id = a2s_new_id(&b);
    label_id = a2s_new_id(&b);
    if (fn->stage == A2S_STAGE_VERTEX) {
        in_id = a2s_new_id(&b);
        loaded_id = a2s_new_id(&b);
    } else {
        int i;
        for (i = 0; i < 4; i++)
            comp_id[i] = a2s_new_id(&b);
        color_id = a2s_new_id(&b);
    }
    out_id = a2s_new_id(&b);

    rc = a2s_capability(&b, SPV_CAP_SHADER);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_memory_model(&b, SPV_ADDR_LOGICAL, SPV_MEM_GLSL450);
    if (rc != A2S_OK)
        return rc;

    if (fn->stage == A2S_STAGE_VERTEX) {
        iface[0] = in_id;
        iface[1] = out_id;
        iface_count = 2;
    } else {
        iface[0] = out_id;
        iface_count = 1;
    }
    rc = a2s_entry_point(&b, model, entry_id, fn->entry_name, iface,
                         iface_count);
    if (rc != A2S_OK)
        return rc;
    if (fn->stage == A2S_STAGE_FRAGMENT) {
        rc = a2s_execution_mode(&b, entry_id,
                                SPV_EXEC_MODE_ORIGIN_UPPER_LEFT);
        if (rc != A2S_OK)
            return rc;
    }

    rc = a2s_name(&b, entry_id, fn->entry_name);
    if (rc != A2S_OK)
        return rc;
    if (fn->stage == A2S_STAGE_VERTEX) {
        rc = a2s_name(&b, in_id, "in_pos");
        if (rc != A2S_OK)
            return rc;
        rc = a2s_name(&b, out_id, "gl_Position");
        if (rc != A2S_OK)
            return rc;
        rc = a2s_decorate(&b, in_id, SPV_DECORATION_LOCATION,
                           fn->in_location);
        if (rc != A2S_OK)
            return rc;
        rc = a2s_decorate(&b, out_id, SPV_DECORATION_BUILT_IN,
                           SPV_BUILT_IN_POSITION);
        if (rc != A2S_OK)
            return rc;
    } else {
        rc = a2s_name(&b, out_id, "out_color");
        if (rc != A2S_OK)
            return rc;
        rc = a2s_decorate(&b, out_id, SPV_DECORATION_LOCATION, 0);
        if (rc != A2S_OK)
            return rc;
    }

    rc = a2s_type_void(&b, void_ty);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_type_float(&b, float_ty, 32);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_type_vector(&b, vec4_ty, float_ty, 4);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_type_pointer(&b, ptr_in_vec4, SPV_STORAGE_INPUT, vec4_ty);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_type_pointer(&b, ptr_out_vec4, SPV_STORAGE_OUTPUT, vec4_ty);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_type_function(&b, fn_ty, void_ty);
    if (rc != A2S_OK)
        return rc;

    if (fn->stage == A2S_STAGE_FRAGMENT) {
        int i;
        for (i = 0; i < 4; i++) {
            rc = a2s_constant(&b, float_ty, comp_id[i],
                              a2s_float_bits(fn->color[i]));
            if (rc != A2S_OK)
                return rc;
        }
        rc = a2s_constant_composite(&b, vec4_ty, color_id, comp_id, 4);
        if (rc != A2S_OK)
            return rc;
    }

    /* Variables. The vertex input lives in Input storage, the output
     * in Output storage. The fragment path has no input. */
    if (fn->stage == A2S_STAGE_VERTEX) {
        rc = a2s_variable(&b, ptr_in_vec4, in_id, SPV_STORAGE_INPUT);
        if (rc != A2S_OK)
            return rc;
    }
    rc = a2s_variable(&b, ptr_out_vec4, out_id, SPV_STORAGE_OUTPUT);
    if (rc != A2S_OK)
        return rc;

    /* Function body. */
    rc = a2s_function(&b, void_ty, entry_id, fn_ty);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_label(&b, label_id);
    if (rc != A2S_OK)
        return rc;
    if (fn->stage == A2S_STAGE_VERTEX) {
        rc = a2s_load(&b, vec4_ty, loaded_id, in_id);
        if (rc != A2S_OK)
            return rc;
        rc = a2s_store(&b, out_id, loaded_id);
        if (rc != A2S_OK)
            return rc;
    } else {
        rc = a2s_store(&b, out_id, color_id);
        if (rc != A2S_OK)
            return rc;
    }
    rc = a2s_return(&b);
    if (rc != A2S_OK)
        return rc;
    rc = a2s_function_end(&b);
    if (rc != A2S_OK)
        return rc;

    words[3] = b.next_id; /* bound: one past the highest id used */
    *out_count = b.count;
    return A2S_OK;
}

const char *a2s_error_string(int err)
{
    if (err == A2S_OK)
        return "ok";
    if (err == A2S_ERR_BAD_ARG)
        return "bad argument";
    if (err == A2S_ERR_OVERFLOW)
        return "output buffer too small";
    if (err == A2S_ERR_BAD_STAGE)
        return "stage not implemented in this path";
    return "unknown error";
}
