/* metallib.c - freestanding C reader for the Apple .metallib container.
 *
 * See metallib.h for the format. Every offset is bounds checked against
 * the file size recorded in the header; no read can run past the buffer.
 * No libc, no allocation: names and bitcode blobs point into the caller's
 * buffer.
 */
#include "metallib.h"
#include "a2s_common.h"

/* Tag codes as little-endian uint32, e.g. "NAME" is 'N' | 'A'<<8 |
 * 'M'<<16 | 'E'<<24. Written out in hex: multi-character constants are
 * implementation-defined in C, so the values are spelled explicitly. */
#define TAG_NAME 0x454d414eu /* "NAME" */
#define TAG_MDSZ 0x5a53444du /* "MDSZ" */
#define TAG_TYPE 0x45505954u /* "TYPE" */
#define TAG_HASH 0x48534148u /* "HASH" */
#define TAG_OFFT 0x5446464fu /* "OFFT" */
#define TAG_SOFF 0x46464f53u /* "SOFF" */
#define TAG_VERS 0x53524556u /* "VERS" */
#define TAG_LAYR 0x5259414cu /* "LAYR" */
#define TAG_TESS 0x53534554u /* "TESS" */
#define TAG_ENDT 0x54444e45u /* "ENDT" */

/* True when [off, off+size) lies inside a file of extent bytes.
 * Overflow-safe: off and size are uint64, extent is a uint32 widened. */
static int section_ok(uint64_t off, uint64_t size, uint64_t extent)
{
    if (off > extent)
        return 0;
    if (size > extent - off)
        return 0;
    return 1;
}

static int parse_header(const uint8_t *data, uint32_t size, ml_header_t *h,
                        uint32_t *extent_out)
{
    uint64_t file_size;

    if (size < ML_HEADER_SIZE)
        return ML_ERR_TOO_SMALL;
    if (a2s_rd32le(data) != ML_MAGIC)
        return ML_ERR_BAD_MAGIC;

    h->platform = a2s_rd16le(data + 4);
    h->version_major = a2s_rd16le(data + 6);
    h->version_minor = a2s_rd16le(data + 8);
    h->lib_type = data[10];
    h->target_os = data[11];
    h->os_major = a2s_rd16le(data + 12);
    h->os_minor = a2s_rd16le(data + 14);
    h->file_size = a2s_rd64le(data + 16);
    h->func_list_off = a2s_rd64le(data + 24);
    h->func_list_size = a2s_rd64le(data + 32);
    h->pub_meta_off = a2s_rd64le(data + 40);
    h->pub_meta_size = a2s_rd64le(data + 48);
    h->priv_meta_off = a2s_rd64le(data + 56);
    h->priv_meta_size = a2s_rd64le(data + 64);
    h->bitcode_off = a2s_rd64le(data + 72);
    h->bitcode_size = a2s_rd64le(data + 80);

    file_size = h->file_size;
    if (file_size < ML_HEADER_SIZE)
        return ML_ERR_BAD_FILE_SIZE;
    if (file_size > (uint64_t)size)
        return ML_ERR_BAD_FILE_SIZE;
    *extent_out = (uint32_t)file_size;

    if (!section_ok(h->func_list_off, h->func_list_size, file_size))
        return ML_ERR_BAD_SECTION;
    if (!section_ok(h->pub_meta_off, h->pub_meta_size, file_size))
        return ML_ERR_BAD_SECTION;
    if (!section_ok(h->priv_meta_off, h->priv_meta_size, file_size))
        return ML_ERR_BAD_SECTION;
    if (!section_ok(h->bitcode_off, h->bitcode_size, file_size))
        return ML_ERR_BAD_SECTION;
    if (h->func_list_size < 4)
        return ML_ERR_BAD_SECTION;
    return ML_OK;
}

/* Parse one tag group at [pos, pos+gsize). Fills fn, which the caller
 * pre-initialised. Returns ML_OK or a negative error. */
static int parse_group(const uint8_t *data, uint64_t pos, uint64_t gsize,
                       ml_function_t *fn)
{
    uint64_t gend = pos + gsize;
    uint64_t tpos = pos + 4; /* skip the group's own size field */
    int saw_endt = 0;

    while (tpos < gend) {
        uint32_t code;
        uint16_t tsize;
        const uint8_t *payload;

        if (tpos + 4 > gend)
            return ML_ERR_BAD_TAG;
        code = a2s_rd32le(data + tpos);
        if (code == TAG_ENDT) {
            /* ENDT is a bare 4-byte code: no size field, no payload.
             * See the note in metallib.h. */
            tpos += 4;
            saw_endt = 1;
            break;
        }
        if (tpos + 6 > gend)
            return ML_ERR_BAD_TAG;
        tsize = a2s_rd16le(data + tpos + 4);
        if ((uint64_t)tsize > gend - (tpos + 6))
            return ML_ERR_BAD_TAG;
        payload = data + tpos + 6;

        if (code == TAG_NAME) {
            uint32_t i;
            uint32_t nlen = 0;
            int found_nul = 0;
            for (i = 0; i < tsize; i++) {
                if (payload[i] == 0) {
                    nlen = i;
                    found_nul = 1;
                    break;
                }
            }
            if (!found_nul || nlen == 0)
                return ML_ERR_NO_NAME;
            fn->name = (const char *)payload;
            fn->name_len = nlen;
        } else if (code == TAG_MDSZ) {
            if (tsize != 8)
                return ML_ERR_BAD_TAG;
            fn->bitcode_size = a2s_rd64le(payload);
            fn->has_mdsz = 1;
        } else if (code == TAG_TYPE) {
            if (tsize < 1)
                return ML_ERR_BAD_TAG;
            fn->func_type = payload[0];
        } else if (code == TAG_HASH) {
            if (tsize != 32)
                return ML_ERR_BAD_TAG;
            a2s_memcpy(fn->hash, payload, 32);
            fn->has_hash = 1;
        } else if (code == TAG_OFFT) {
            if (tsize != 24)
                return ML_ERR_BAD_TAG;
            fn->offt[0] = a2s_rd64le(payload);
            fn->offt[1] = a2s_rd64le(payload + 8);
            fn->offt[2] = a2s_rd64le(payload + 16);
            fn->has_offt = 1;
        } else if (code == TAG_SOFF) {
            if (tsize != 8)
                return ML_ERR_BAD_TAG;
            fn->soff = a2s_rd64le(payload);
        } else if (code == TAG_VERS) {
            if (tsize != 8)
                return ML_ERR_BAD_TAG;
            fn->air_major = a2s_rd16le(payload);
            fn->air_minor = a2s_rd16le(payload + 2);
            fn->lang_major = a2s_rd16le(payload + 4);
            fn->lang_minor = a2s_rd16le(payload + 6);
        } else if (code == TAG_LAYR) {
            if (tsize < 1)
                return ML_ERR_BAD_TAG;
            fn->layr = payload[0];
        } else if (code == TAG_TESS) {
            if (tsize < 1)
                return ML_ERR_BAD_TAG;
            fn->tess = payload[0];
        }
        /* Unknown tags are skipped: the format is extensible and
         * future tools may add tags we do not know. */
        tpos += 6 + tsize;
    }
    if (!saw_endt)
        return ML_ERR_NO_ENDT;
    if (fn->name == NULL)
        return ML_ERR_NO_NAME;
    return ML_OK;
}

/* Resolve absolute bitcode offsets and sizes. Functions with an OFFT tag
 * use its bitcode-relative offset; functions without one fall back to the
 * sequential layout (blobs concatenated in function order from the
 * section start), which is how the 2015-era files are laid out.
 * Functions without an MDSZ tag fall back to the next function's
 * bitcode offset minus this one, or the rest of the bitcode section for
 * the last function (YuAo parser behaviour). */
static int resolve_bitcode(const uint8_t *data, ml_library_t *lib)
{
    uint64_t running = 0;
    uint32_t i;

    for (i = 0; i < lib->count; i++) {
        ml_function_t *fn = &lib->funcs[i];
        uint64_t rel;

        if (fn->has_offt) {
            rel = fn->offt[2];
        } else {
            rel = running;
        }
        if (!fn->has_mdsz) {
            uint64_t next_rel = lib->header.bitcode_size;
            uint32_t j;
            /* Next function that has an OFFT bitcode offset. */
            for (j = i + 1; j < lib->count; j++) {
                if (lib->funcs[j].has_offt) {
                    next_rel = lib->funcs[j].offt[2];
                    break;
                }
            }
            if (next_rel < rel)
                return ML_ERR_BAD_BITCODE;
            fn->bitcode_size = next_rel - rel;
        }
        if (rel > lib->header.bitcode_size)
            return ML_ERR_BAD_BITCODE;
        if (fn->bitcode_size > lib->header.bitcode_size - rel)
            return ML_ERR_BAD_BITCODE;
        fn->bitcode_off = lib->header.bitcode_off + rel;
        running += fn->bitcode_size;

        fn->bitcode_ok = 0;
        if (fn->bitcode_size >= 4) {
            uint32_t magic = a2s_rd32le(data + fn->bitcode_off);
            if (magic == ML_BITCODE_WRAPPER_MAGIC ||
                magic == ML_BITCODE_MAGIC)
                fn->bitcode_ok = 1;
        }
    }
    return ML_OK;
}

int ml_open(const uint8_t *data, uint32_t size, ml_library_t *lib)
{
    uint32_t extent = 0;
    uint64_t pos;
    uint32_t count;
    uint32_t i;
    int rc;

    if (data == NULL || lib == NULL || size < ML_HEADER_SIZE)
        return ML_ERR_TOO_SMALL;

    a2s_memset(lib, 0, sizeof(*lib));
    rc = parse_header(data, size, &lib->header, &extent);
    if (rc != ML_OK)
        return rc;

    /* Groups are bounded by the file extent, NOT by the header's
     * function-list size: that size lands on the last group's ENDT
     * (see metallib.h), so a group bound check against it would reject
     * real files. Groups are self-delimiting (count + ENDT). */
    count = a2s_rd32le(data + lib->header.func_list_off);
    if (count > ML_MAX_FUNCTIONS)
        return ML_ERR_TOO_MANY;

    pos = lib->header.func_list_off + 4;
    for (i = 0; i < count; i++) {
        uint32_t gsize;
        ml_function_t *fn = &lib->funcs[i];

        if (pos + 4 > extent)
            return ML_ERR_TRUNCATED;
        gsize = a2s_rd32le(data + pos);
        if (gsize < 4)
            return ML_ERR_BAD_TAG;
        if ((uint64_t)gsize > extent - pos)
            return ML_ERR_TRUNCATED;
        fn->func_type = 0xff;
        rc = parse_group(data, pos, gsize, fn);
        if (rc != ML_OK)
            return rc;
        pos += gsize;
    }
    lib->count = count;
    lib->data = data;
    lib->size = extent;

    return resolve_bitcode(data, lib);
}

int ml_function_bitcode(const ml_library_t *lib, uint32_t index,
                        const uint8_t **out, uint32_t *out_size)
{
    const ml_function_t *fn;

    if (lib == NULL || out == NULL || out_size == NULL)
        return ML_ERR_BAD_INDEX;
    if (index >= lib->count)
        return ML_ERR_BAD_INDEX;
    fn = &lib->funcs[index];
    if (fn->bitcode_size == 0)
        return ML_ERR_BAD_BITCODE;
    if (fn->bitcode_off > lib->size)
        return ML_ERR_BAD_BITCODE;
    if (fn->bitcode_size > lib->size - (uint32_t)fn->bitcode_off)
        return ML_ERR_BAD_BITCODE;
    *out = lib->data + fn->bitcode_off;
    *out_size = (uint32_t)fn->bitcode_size;
    return ML_OK;
}

int ml_find(const ml_library_t *lib, const char *name)
{
    uint32_t i;

    if (lib == NULL || name == NULL)
        return -1;
    for (i = 0; i < lib->count; i++) {
        if (a2s_strcmp(lib->funcs[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

const char *ml_platform_name(uint16_t platform)
{
    if (platform == ML_PLATFORM_IOS)
        return "iOS";
    if (platform == ML_PLATFORM_MACOS)
        return "macOS";
    return "unknown";
}

const char *ml_lib_type_name(uint8_t lib_type)
{
    if (lib_type == ML_TYPE_EXECUTABLE)
        return "executable";
    if (lib_type == ML_TYPE_CORE_IMAGE)
        return "Core Image";
    if (lib_type == ML_TYPE_DYNAMIC)
        return "dynamic";
    if (lib_type == ML_TYPE_SYMBOL_COMPANION)
        return "symbol companion";
    return "unknown";
}

const char *ml_target_os_name(uint8_t target_os)
{
    if (target_os == ML_OS_MACOS)
        return "macOS";
    if (target_os == ML_OS_IOS)
        return "iOS";
    if (target_os == ML_OS_TVOS)
        return "tvOS";
    if (target_os == ML_OS_WATCHOS)
        return "watchOS";
    if (target_os == ML_OS_BRIDGEOS)
        return "bridgeOS";
    if (target_os == ML_OS_MACCATALYST)
        return "macCatalyst";
    if (target_os == ML_OS_IOS_SIMULATOR)
        return "iOS Simulator";
    if (target_os == ML_OS_TVOS_SIMULATOR)
        return "tvOS Simulator";
    if (target_os == ML_OS_WATCHOS_SIMULATOR)
        return "watchOS Simulator";
    if (target_os == ML_OS_UNKNOWN)
        return "unknown";
    return "unknown";
}

const char *ml_func_type_name(uint8_t func_type)
{
    if (func_type == ML_FUNC_VERTEX)
        return "vertex";
    if (func_type == ML_FUNC_FRAGMENT)
        return "fragment";
    if (func_type == ML_FUNC_KERNEL)
        return "kernel";
    if (func_type == ML_FUNC_UNQUALIFIED)
        return "unqualified";
    if (func_type == ML_FUNC_VISIBLE)
        return "visible";
    if (func_type == ML_FUNC_EXTERN)
        return "extern";
    if (func_type == ML_FUNC_INTERSECTION)
        return "intersection";
    return "unknown";
}

const char *ml_error_string(int err)
{
    if (err == ML_OK)
        return "ok";
    if (err == ML_ERR_TOO_SMALL)
        return "buffer too small for the metallib header";
    if (err == ML_ERR_BAD_MAGIC)
        return "bad magic, not a metallib file";
    if (err == ML_ERR_BAD_FILE_SIZE)
        return "header file size disagrees with the buffer";
    if (err == ML_ERR_BAD_SECTION)
        return "a section offset/size runs past the file";
    if (err == ML_ERR_TRUNCATED)
        return "function list or tag group is truncated";
    if (err == ML_ERR_TOO_MANY)
        return "more functions than ML_MAX_FUNCTIONS";
    if (err == ML_ERR_BAD_TAG)
        return "a tag runs past its group or has a bad size";
    if (err == ML_ERR_NO_ENDT)
        return "a tag group does not end with ENDT";
    if (err == ML_ERR_NO_NAME)
        return "a function entry has no usable NAME tag";
    if (err == ML_ERR_BAD_BITCODE)
        return "bitcode offset/size runs past the bitcode section";
    if (err == ML_ERR_BAD_INDEX)
        return "function index out of range";
    return "unknown error";
}
