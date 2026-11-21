/*
 * iokit.h: IOKit user-space to kernel wire emulation for ioswine
 *
 *
 * Emulates the Mach/MIG side of IOKit inside libsystem_kernel so
 * Apple's unmodified IOKit.framework can find services, open user
 * clients and call external methods. The kernel side is a set of
 * kobject ports (see PORT_KOBJECT_IOKIT below) dispatching on the
 * MIG routine id in msgh_id. The object model is deliberately
 * small: a service registry by class name with property dicts,
 * connections (io_connect_t) opened on services, and iterators
 * (io_iterator_t) for matching-services results.
 *
 * Style: freestanding C, same as xpcwire.c / svchost.c / fakeboard.c.
 * No libc. No function-pointer tables in static data anywhere
 * (arm64e ld64.lld rejects signed pointers in static data): all
 * dispatch in this file's implementation is if/else-if chains.
 * Static const tables of plain integers and strings are fine and
 * are used for the MIG routine ids and the IOReturn codes.
 *
 * Sources and [guess] list (read before trusting any wire value).
 * The [src:] tags below point at public sources.
 *
 * - The MIG subsystem id is 2800 and the routine table is counted
 *   in file order from xnu-1228.15.4 osfmk/device/device.defs
 *   [src: https://newosxbook.com/code/xnu-1228.15.4/osfmk/device/device.defs].
 *   The ids in the table below are that file's ids. Later xnu versions
 *   append routines above 2870 and do not reorder the old ones,
 *   so these ids are the baseline. The three post-2009 _bin
 *   routines have no public id and are given
 *   placeholder ids 2871-2873 below, marked PLACEHOLDER: renumber
 *   them from the first device trace (one #define each).
 * - io_connect_method is 2865, io_connect_async_method 2866,
 *   io_service_open_extended 2862, io_connect_map_memory_into_task
 *   2863 [src: same .defs file]. IOConnectCallMethod wraps
 *   io_connect_method [src: Project Zero, IOKit MIG overview].
 * - MIG replies use msgh_id = request id + 100, an NDR record,
 *   then a kern_return_t RetCode [src: Mac OS X Internals,
 *   section 9.6: "the reply identifier is conventionally the sum
 *   of the request identifier and the number 100"]. The NDR bytes
 *   on arm64 little-endian iOS are int_rep = 1, char_rep = 0,
 *   float_rep = 0 [guess: standard NDR values].
 * - io_connect_method request, after header, descriptors and
 *   NDR: selector u32, scalar_inputCnt u32 + that many u64,
 *   inband_inputCnt u32 + that many bytes, ool_input u64,
 *   ool_input_size u64, then the CountInOut capacities
 *   inband_outputCnt u32, scalar_outputCnt u32, ool_output u64,
 *   ool_output_size u64. Reply: RetCode, inband_outputCnt u32 +
 *   that many bytes, scalar_outputCnt u32 + that many u64,
 *   ool_output_size u64. The order is the .defs declaration order
 *   (inband_output is declared before scalar_output, device.defs
 *   lines 611-612) [src: the .defs text quoted in the flanker017
 *   TOCTOU writeup, which reproduces device.defs verbatim; MIG
 *   emits message fields in declaration order]. (a) ool_output is declared
 *   `in`, so it appears in the request only, never in the reply;
 *   the reply carries only ool_output_size (inout); (b) the
 *   inband/scalar order follows declaration order.
 *   The first device trace MUST confirm the order: for a create
 *   call the inband count will be ~3016 and the scalar count 0-2,
 *   so the two orders are immediately distinguishable. If the
 *   trace shows scalar-first, swap the two counts (request) and
 *   the two output blocks (reply) in iokit.c's method handlers.
 * - io_connect_async_method (2866) is the same call with
 *   wake_port (port descriptor) and reference (io_async_ref64_t,
 *   fixed 8 x u64, no count field) at the front, before the
 *   selector [src: same .defs file].
 * - io_service_open_extended (2862): owningTask port descriptor
 *   plus a properties io_buf_ptr_t OOL descriptor; reply RetCode,
 *   result u32, connection port descriptor (MOVE_SEND)
 *   [src: same .defs file].
 * - io_registry_entry_get_property (2805) returns its value as an
 *   io_buf_ptr_t OOL descriptor [src: same .defs file]. The OOL reply
 *   data lives in a static scratch
 *   buffer (IOKIT_OOL_SCRATCH_SIZE); ipc.c's kobject reply path
 *   must transfer OOL descriptors, or the integration must copy
 *   the bytes. This is a known integration point, not a solved
 *   one: the host test reads the OOL address directly (in-process).
 * - OSSerializeBinary: signature 0x000000d3, tags in the top byte
 *   (dictionary 0x01, array 0x02, set 0x03, number 0x04, symbol
 *   0x08, string 0x09, data 0x0a, boolean 0x0b, object reference
 *   0x0c), low 24 bits holding the count or byte length, numbers
 *   followed by 4 or 8 little-endian bytes, strings/symbols/data
 *   padded to a 4-byte boundary, dictionary keys usually Symbols
 *   but String keys appear in the worked example and must be
 *   accepted [src: Pegasus writeup].
 *   The EndCollection bit (0x80000000) is set on the last value of
 *   a collection and on a container's own tag when the container
 *   is the last value of its parent (or the root) [src: same
 *   writeup, worked example]. The reader here uses the COUNT as
 *   authoritative and ignores the bit (the worked example sets it
 *   on a non-last value, so the bit cannot be trusted for
 *   termination); the writer sets it per the rule above, so a
 *   reader that uses the bit also decodes our output correctly.
 *   Numbers carry no signedness flag: the tag's low bits are the
 *   bit length (32 or 64) and the value is little-endian
 *   [src: same writeup]. The object-reference index is 0-based
 *   into the decode-order object table [guess].
 * - Matching dictionaries: IOServiceMatching sets the
 *   IOProviderClass key to the class name [src: IOKitUser matching keys].
 *   The matcher here also accepts
 *   IOClass and IONameMatch, and an IOPropertyMatch sub-dict.
 *   Modern IOKitUser sends matching dicts as OSSerializeBinary
 *   through the _bin/_ool routines; the original 2804 takes an XML
 *   plist string in io_string_t (max 512 bytes). Both forms are accepted here.
 * - IOSurface create and set_value inputs are XML plists, and
 *   OSUnserializeXML auto-detects the binary signature and hands
 *   off to the binary parser. The
 *   osser_decode_auto entry point below implements exactly that
 *   detection. Registry property replies and get_value output are
 *   binary.
 * - kIOReturn codes are the public IOReturn.h values
 *   [src: IOKit/IOReturn.h, public header].
 * - IOObjectRelease / IOObjectRetain are local port deallocations,
 *   not MIG routines; there is no
 *   io_object_release/retain routine, and no io_object_is_alias.
 *   Connections are closed with io_service_close (2816).
 * - _iokit_user_client_trap (x16 = -100, one svc #0x80 in
 *   IOKit.framework) is the legacy trap path for IOConnectTrap0..5;
 *   it is NOT emulated here (out of scope for v1: IOSurface
 *   .framework and QuartzCore use IOConnectCallMethod, not the
 *   traps).
 * - CA_FORCE_LOCAL_SERVER=1: no public source was found for that
 *   exact string at all; its existence and behaviour are both
 *   [guess]. Nothing in this file asserts it as fact.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "ipc.h"

/*
 * Required ipc.c addition (does NOT exist yet; do not apply here).
 * Mirrors the svchost patch.
 *
 *   1. In ipc.h, after PORT_KOBJECT_BKS_DISPLAY (6):
 *        #define PORT_KOBJECT_IOKIT 8
 *   2. In ipc.c, a port creator (mirrors ipc_svchost_port_create):
 *        mach_port_name_t ipc_iokit_port_create(void);
 *      Allocate one receive port of kind PORT_KOBJECT_IOKIT in the
 *      caller task and return its name, or MACH_PORT_NULL on
 *      failure. The port's kobject address is not used: dispatch
 *      resolves the object from hdr->msgh_remote_port.
 *   3. In the ipc.c dispatch branch (where PORT_KOBJECT_SVCHOST is
 *      handled), add:
 *        else if (kobject_kind == PORT_KOBJECT_IOKIT) {
 *            handled = iokit_process_msg(hdr, send_size,
 *                                        rep_buf, rep_cap, &rep_size);
 *        }
 *      with a rep_buf of at least IOKIT_MAX_REPLY bytes. The
 *      dispatch runs under ipc_lock, so iokit_process_msg must
 *      follow the svchost handler rules: no ipc_lock takers, no
 *      blocking, no allocation, never drop a send-once reply.
 *   4. host_get_io_master (host kobject, PORT_KOBJECT_HOST) must
 *      return iokit_master_port() as the master port. Until that
 *      hook exists, clients call iokit_master_port() directly.
 *   5. OOL descriptors in kobject replies (io_registry_entry_get_
 *      property and friends return io_buf_ptr_t): ipc.c's reply
 *      path must transfer them (the OOL address points at the
 *      static scratch buffer in iokit.c). If ipc.c cannot carry
 *      OOL in a kobject reply, the integration must copy the
 *      scratch bytes inline instead; the host test reads the OOL
 *      address directly and does not need this.
 */
#ifndef PORT_KOBJECT_IOKIT
#define PORT_KOBJECT_IOKIT 8
#endif

/* Limits. */
#define IOKIT_MAX_SERVICES     32
#define IOKIT_MAX_CONNECTIONS  32
#define IOKIT_MAX_ITERATORS    16
#define IOKIT_CLASS_MAX        64
#define IOKIT_MAX_PROPS        4096  /* bytes of OSSerializeBinary per service */
#define IOKIT_MAX_REPLY        8192  /* total MIG reply Mach message */
#define IOKIT_MAX_MIG_MSG      8192  /* largest request we parse */
#define IOKIT_MAX_SCALARS      16    /* io_scalar_inband64_t */
#define IOKIT_MAX_STRUCT_IN    4096  /* io_struct_inband_t */
#define IOKIT_MAX_STRING       512   /* io_string_t */
#define IOKIT_MAX_NAME         128   /* io_name_t */
#define IOKIT_MAX_ASYNC_REF    8     /* io_async_ref64_t */
#define IOKIT_OOL_SCRATCH_SIZE 8192  /* static buffer for OOL reply data */

/* IOReturn codes (public IOReturn.h). The MIG RetCode for IOKit
 * routines is a kern_return_t; IOKit-specific failures use these
 * values, which the IOKitUser wrappers pass through unchanged. */
#define kIOReturnSuccess          0
#define kIOReturnError            0xe00002bcU
#define kIOReturnNoMemory         0xe00002bdU
#define kIOReturnNoResources      0xe00002beU
#define kIOReturnIPCError         0xe00002bfU
#define kIOReturnNoDevice         0xe00002c0U
#define kIOReturnNotPrivileged    0xe00002c1U
#define kIOReturnBadArgument      0xe00002c2U
#define kIOReturnLockedRead       0xe00002c3U
#define kIOReturnLockedWrite      0xe00002c4U
#define kIOReturnExclusiveAccess  0xe00002c5U
#define kIOReturnBadMessageID     0xe00002c6U
#define kIOReturnUnsupported      0xe00002c7U
#define kIOReturnVMError          0xe00002c8U
#define kIOReturnInternalError    0xe00002c9U
#define kIOReturnIOError          0xe00002caU
#define kIOReturnCannotLock       0xe00002cbU
#define kIOReturnNotOpen          0xe00002ccU
#define kIOReturnNotReadable      0xe00002cdU
#define kIOReturnNotWritable      0xe00002ceU
#define kIOReturnNotFound         0xe00002cfU
#define kIOReturnNotBusy          0xe00002d0U
#define kIOReturnNoMedia          0xe00002d1U
#define kIOReturnBusy             0xe00002d2U

/* ------------------------------------------------------------------ */
/* MIG routine ids. Subsystem base 2800 [src: xnu-1228.15.4          */
/* device.defs]; ids 2800-2870 are that file's ids, counted in file  */
/* order (Part 1 section 1.3). Routines marked "skip on LP64" in    */
/* the .defs file have no 64-bit userland stub; the framework calls  */
/* the _64 variant instead, but the old id is still consumed, so    */
/* the numbering holds. The reply id is request + 100 [guess].      */
/* ------------------------------------------------------------------ */
#define IOKIT_MIG_BASE 2800U
#define IOKIT_MIG_OBJECT_GET_CLASS                2800U
#define IOKIT_MIG_OBJECT_CONFORMS_TO              2801U
#define IOKIT_MIG_ITERATOR_NEXT                   2802U
#define IOKIT_MIG_ITERATOR_RESET                  2803U
#define IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES   2804U
#define IOKIT_MIG_REGISTRY_GET_PROPERTY           2805U
#define IOKIT_MIG_REGISTRY_CREATE_ITERATOR        2806U
#define IOKIT_MIG_REGISTRY_ITERATOR_ENTER_ENTRY   2807U
#define IOKIT_MIG_REGISTRY_ITERATOR_EXIT_ENTRY    2808U
#define IOKIT_MIG_REGISTRY_ENTRY_FROM_PATH        2809U
#define IOKIT_MIG_REGISTRY_GET_NAME               2810U
#define IOKIT_MIG_REGISTRY_GET_PROPERTIES         2811U
#define IOKIT_MIG_REGISTRY_GET_PROPERTY_BYTES     2812U
#define IOKIT_MIG_REGISTRY_GET_CHILD_ITERATOR     2813U
#define IOKIT_MIG_REGISTRY_GET_PARENT_ITERATOR    2814U
#define IOKIT_MIG_SERVICE_OPEN                    2815U
#define IOKIT_MIG_SERVICE_CLOSE                   2816U
#define IOKIT_MIG_CONNECT_GET_SERVICE             2817U
#define IOKIT_MIG_CONNECT_SET_NOTIFICATION_PORT   2818U /* skip LP64; use 2867 */
#define IOKIT_MIG_CONNECT_MAP_MEMORY              2819U /* skip LP64; use 2863 */
#define IOKIT_MIG_CONNECT_ADD_CLIENT              2820U
#define IOKIT_MIG_CONNECT_SET_PROPERTIES          2821U
#define IOKIT_MIG_CONNECT_METHOD_SCALAR_I_SCALAR_O 2822U /* skip LP64 */
#define IOKIT_MIG_CONNECT_METHOD_SCALAR_I_STRUCT_O 2823U /* skip LP64 */
#define IOKIT_MIG_CONNECT_METHOD_SCALAR_I_STRUCT_I 2824U /* skip LP64 */
#define IOKIT_MIG_CONNECT_METHOD_STRUCT_I_STRUCT_O 2825U /* skip LP64 */
#define IOKIT_MIG_REGISTRY_GET_PATH               2826U
#define IOKIT_MIG_REGISTRY_GET_ROOT_ENTRY         2827U
#define IOKIT_MIG_REGISTRY_SET_PROPERTIES         2828U
#define IOKIT_MIG_REGISTRY_IN_PLANE               2829U
#define IOKIT_MIG_OBJECT_GET_RETAIN_COUNT         2830U
#define IOKIT_MIG_SERVICE_GET_BUSY_STATE          2831U
#define IOKIT_MIG_SERVICE_WAIT_QUIET              2832U
#define IOKIT_MIG_REGISTRY_ENTRY_CREATE_ITERATOR  2833U
#define IOKIT_MIG_ITERATOR_IS_VALID               2834U
#define IOKIT_MIG_MAKE_MATCHING                   2835U
#define IOKIT_MIG_CATALOG_SEND_DATA               2836U
#define IOKIT_MIG_CATALOG_TERMINATE               2837U
#define IOKIT_MIG_CATALOG_GET_DATA                2838U
#define IOKIT_MIG_CATALOG_GET_GEN_COUNT           2839U
#define IOKIT_MIG_CATALOG_MODULE_LOADED           2840U
#define IOKIT_MIG_CATALOG_RESET                   2841U
#define IOKIT_MIG_SERVICE_REQUEST_PROBE           2842U
#define IOKIT_MIG_REGISTRY_GET_NAME_IN_PLANE      2843U
#define IOKIT_MIG_SERVICE_MATCH_PROPERTY_TABLE    2844U
#define IOKIT_MIG_ASYNC_METHOD_SCALAR_I_SCALAR_O  2845U /* skip LP64 */
#define IOKIT_MIG_ASYNC_METHOD_SCALAR_I_STRUCT_O  2846U /* skip LP64 */
#define IOKIT_MIG_ASYNC_METHOD_SCALAR_I_STRUCT_I  2847U /* skip LP64 */
#define IOKIT_MIG_ASYNC_METHOD_STRUCT_I_STRUCT_O  2848U /* skip LP64 */
#define IOKIT_MIG_SERVICE_ADD_NOTIFICATION        2849U /* skip LP64; use 2868 */
#define IOKIT_MIG_SERVICE_ADD_INTEREST_NOTIF      2850U /* skip LP64; use 2869 */
#define IOKIT_MIG_SERVICE_ACK_NOTIFICATION        2851U /* skip LP64 */
#define IOKIT_MIG_CONNECT_GET_NOTIFICATION_SEMAPHORE 2852U
#define IOKIT_MIG_CONNECT_UNMAP_MEMORY            2853U /* skip LP64; use 2864 */
#define IOKIT_MIG_REGISTRY_GET_LOCATION_IN_PLANE  2854U
#define IOKIT_MIG_REGISTRY_GET_PROPERTY_RECURSIVELY 2855U
#define IOKIT_MIG_SERVICE_GET_STATE               2856U
#define IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES_OOL 2857U
#define IOKIT_MIG_SERVICE_MATCH_PROPERTY_TABLE_OOL 2858U
#define IOKIT_MIG_SERVICE_ADD_NOTIFICATION_OOL    2859U /* skip LP64; use 2870 */
#define IOKIT_MIG_OBJECT_GET_SUPERCLASS           2860U
#define IOKIT_MIG_OBJECT_GET_BUNDLE_IDENTIFIER    2861U
#define IOKIT_MIG_SERVICE_OPEN_EXTENDED           2862U
#define IOKIT_MIG_CONNECT_MAP_MEMORY_INTO_TASK    2863U
#define IOKIT_MIG_CONNECT_UNMAP_MEMORY_FROM_TASK  2864U
#define IOKIT_MIG_CONNECT_METHOD                  2865U
#define IOKIT_MIG_CONNECT_ASYNC_METHOD            2866U
#define IOKIT_MIG_CONNECT_SET_NOTIFICATION_PORT_64 2867U
#define IOKIT_MIG_SERVICE_ADD_NOTIFICATION_64     2868U
#define IOKIT_MIG_SERVICE_ADD_INTEREST_NOTIF_64   2869U
#define IOKIT_MIG_SERVICE_ADD_NOTIFICATION_OOL_64 2870U
/* Placeholders: post-2009 _bin routines. No public source gives
 * their ids (Part 1 section 1.4: "unknown, trace it"). 2871-2873
 * are the first free slots above 2870, chosen only so the
 * dispatch has something to match on; renumbering is one #define
 * each once the device trace (or mig on current device.defs)
 * gives the real ids. Do not treat these as established. */
#define IOKIT_MIG_SERVICE_GET_MATCHING_SERVICE_BIN  2871U /* PLACEHOLDER */
#define IOKIT_MIG_SERVICE_GET_MATCHING_SERVICES_BIN 2872U /* PLACEHOLDER */
#define IOKIT_MIG_REGISTRY_GET_PROPERTY_BIN         2873U /* PLACEHOLDER */

/* Service kinds. The kind decides which io_connect_method handler
 * runs for a connection opened on the service; dispatch is an
 * if-chain in iokit.c, never a function-pointer table. Generic
 * services answer io_connect_method with kIOReturnUnsupported. */
#define IOKIT_SERVICE_KIND_GENERIC          0U
#define IOKIT_SERVICE_KIND_IOSURFACE_ROOT   1U

/* ------------------------------------------------------------------ */
/* OSSerializeBinary + XML plist (osser_*).                            */
/* ------------------------------------------------------------------ */

#define OSSER_SIGNATURE 0x000000d3U
#define OSSER_MAX_DEPTH 16
#define OSSER_ERR_OK           0
#define OSSER_ERR_NOSPACE     -1
#define OSSER_ERR_TRUNCATED   -2
#define OSSER_ERR_BAD_TAG     -3
#define OSSER_ERR_BAD_SIG     -4
#define OSSER_ERR_NOT_FOUND   -5
#define OSSER_ERR_BAD_TYPE    -6
#define OSSER_ERR_OVERFLOW    -7
#define OSSER_ERR_BAD_REF     -8
#define OSSER_ERR_BAD_XML     -9

typedef enum {
    OSSER_DICT = 1,
    OSSER_ARRAY,
    OSSER_STRING,
    OSSER_NUMBER,
    OSSER_BOOL,
    OSSER_DATA,
    OSSER_REAL
} osser_type_t;

/* One decoded value. String and data bytes point into the input
 * buffer (zero-copy) for binary input; for XML input they point
 * into the pool's text buffer (entity-decoded, NUL terminated for
 * convenience, length excludes the NUL). Number values are stored
 * raw in num with num_bits 32 or 64 (binary) or 64 (XML integer);
 * there is no signedness flag in the binary format, so
 * osser_as_int64 reinterprets the bit pattern. Real values store
 * the double in dbl. Dict/array children are a run of osser_value
 * structs from the reader pool, keys a parallel run of osser_key
 * structs (dicts only). A binary reference (kOSSerializeObject)
 * decodes as a struct copy of the referenced node, so the tree
 * never aliases pool nodes in a way the caller must track. */
typedef struct {
    const uint8_t *ptr;
    uint32_t len;
} osser_key;

typedef struct osser_value {
    osser_type_t type;
    uint64_t num;         /* number: raw value bits */
    uint32_t num_bits;    /* number: 32 or 64 */
    double dbl;           /* real: value */
    int boolean;          /* bool: 0 or 1 */
    const uint8_t *bytes; /* string/data bytes */
    uint32_t len;         /* string/data byte length (excl. NUL) */
    struct osser_value *items; /* dict/array children, pool run */
    osser_key *keys;      /* dict keys, parallel to items; NULL for arrays */
    uint32_t count;       /* dict entries / array elements */
} osser_value;

/* Reader pool: the caller supplies the backing arrays. Sizing rule
 * for a matching dict or a surface properties dict: 128 value
 * nodes, 128 keys, 128 object references and 2048 text bytes are
 * far more than enough (a 1080x2400 create dict decodes to about
 * 20 nodes). The text buffer is only used by the XML parser. */
typedef struct {
    osser_value *vals;
    uint32_t vals_cap;
    uint32_t vals_used;
    osser_key *keys;
    uint32_t keys_cap;
    uint32_t keys_used;
    osser_value **objs;   /* object table for binary references, decode order */
    uint32_t objs_cap;
    uint32_t objs_used;
    uint8_t *text;        /* bump buffer for XML-decoded strings */
    uint32_t text_cap;
    uint32_t text_used;
} osser_pool;

/* Decode an OSSerializeBinary blob. The root may be any type.
 * Returns OSSER_ERR_OK or a negative OSSER_ERR_*. On success
 * *root_out points at the root node in the pool; the pool arrays
 * must stay alive while the tree is used. */
int osser_decode(const uint8_t *buf, uint32_t len, osser_pool *pool,
                 osser_value **root_out);

/* Decode an XML plist document (<plist><dict>... or any value)
 * into the same value model. Supports dict, array, string,
 * integer, real, true, false, data (base64), date (as string).
 * Returns OSSER_ERR_OK or a negative OSSER_ERR_* (BAD_XML for
 * malformed input). */
int osser_xml_decode(const uint8_t *buf, uint32_t len, osser_pool *pool,
                     osser_value **root_out);

/* Auto-detect: binary signature 0xd3 at the start means binary,
 * anything else is tried as XML. This mirrors OSUnserializeXML,
 * which checks for the binary signature and hands off to the
 * binary parser. */
int osser_decode_auto(const uint8_t *buf, uint32_t len, osser_pool *pool,
                      osser_value **root_out);

/* Dictionary lookup by NUL-terminated key. Returns OSSER_ERR_OK or
 * OSSER_ERR_NOT_FOUND / OSSER_ERR_BAD_TYPE. */
int osser_dict_find(const osser_value *dict, const char *key,
                    const osser_value **out);

/* Typed getters. osser_as_uint64 accepts numbers (and bools as
 * 0/1); osser_as_int64 reinterprets the number bits as signed;
 * osser_as_double accepts reals and numbers; osser_as_string
 * accepts strings (binary symbol and string alike, and XML
 * strings). String bytes are NOT NUL terminated in general for
 * binary input; the length is returned. */
int osser_as_uint64(const osser_value *v, uint64_t *out);
int osser_as_int64(const osser_value *v, int64_t *out);
int osser_as_double(const osser_value *v, double *out);
int osser_as_bool(const osser_value *v, int *out);
int osser_as_string(const osser_value *v, const uint8_t **out,
                    uint32_t *len_out);

/* Writer: caller supplied buffer, never written past cap. The
 * writer never emits object references: every string is written
 * out in full, which any conforming reader accepts. Counts are
 * enforced: begin_dict/begin_array take the entry count, and
 * end_dict/end_array fail when the count does not match what was
 * written. The EndCollection bit is set on the last value of each
 * collection and on a container's own tag when the container is
 * the last value of its parent (or the root), per Part 1 section
 * 2.2. Reals are rejected (the binary format has no real type);
 * encode them as 64-bit numbers of the bit pattern instead, or
 * keep them out of binary output. */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    int err;
    struct {
        uint32_t kind;      /* OSSER_DICT or OSSER_ARRAY */
        uint32_t remaining; /* entries still to write at this level */
        int need_value;     /* dict level: a key was written, value due */
    } stack[OSSER_MAX_DEPTH];
    uint32_t depth;
    int root_done;      /* a complete root value was written */
} osser_writer;

int osser_w_init(osser_writer *w, uint8_t *buf, uint32_t cap);
uint32_t osser_w_len(const osser_writer *w);
int osser_w_error(const osser_writer *w);
int osser_w_begin_dict(osser_writer *w, uint32_t count);
int osser_w_end_dict(osser_writer *w);
int osser_w_begin_array(osser_writer *w, uint32_t count);
int osser_w_end_array(osser_writer *w);
/* Write a dictionary key (symbol). Must be followed by exactly one
 * value (scalar, data, or a begin_dict/begin_array). */
int osser_w_key(osser_writer *w, const char *key);
/* Same, for a key that is not NUL terminated (raw bytes + length). */
int osser_w_key_bytes(osser_writer *w, const uint8_t *p, uint32_t len);
int osser_w_string(osser_writer *w, const char *s);
int osser_w_bytes(osser_writer *w, const uint8_t *p, uint32_t len);
int osser_w_u32(osser_writer *w, uint32_t v);
int osser_w_u64(osser_writer *w, uint64_t v);
int osser_w_s64(osser_writer *w, int64_t v);
int osser_w_bool(osser_writer *w, int v);
int osser_w_data(osser_writer *w, const uint8_t *p, uint32_t len);

/* Re-encode a decoded value (any type except real) with the
 * writer, recursively. Used to answer get_property_bin from a
 * decoded service property dict. Returns OSSER_ERR_OK or a
 * negative OSSER_ERR_*. */
int osser_w_value(osser_writer *w, const osser_value *v);

/* ------------------------------------------------------------------ */
/* Object model.                                                       */
/* ------------------------------------------------------------------ */

/* One registered service. props is the OSSerializeBinary property
 * dict blob (copied in at registration); props_len 0 means no
 * properties. kind selects the io_connect_method handler. refcount
 * counts io_object_retain/release references from user space;
 * IOObjectRelease is a local port deallocation, not a MIG call,
 * so the count here is only for io_object_get_retain_count
 * queries and the registry entry itself is never destroyed by a
 * release (services are torn down only by an explicit unregister,
 * which v1 does not implement). */
typedef struct {
    int in_use;
    mach_port_name_t port;
    char class_name[IOKIT_CLASS_MAX];
    uint8_t props[IOKIT_MAX_PROPS];
    uint32_t props_len;
    uint32_t kind;
    uint32_t refcount;
} iokit_service;

/* One open user-client connection. async_port/async_ref are
 * recorded from io_connect_async_method; v1 never delivers an
 * async completion (the call itself runs synchronously).
 * notify_port/notify_type/notify_ref are recorded from
 * io_connect_set_notification_port(_64); v1 never delivers a
 * notification. memory_addr / memory_size are the mapping
 * io_connect_map_memory returns for this connection (set by
 * iokit_connection_set_memory, used by generic services; the
 * IOSurface root client resolves the surface id per call instead,
 * see iosurface.h). */
typedef struct {
    int in_use;
    mach_port_name_t port;
    uint32_t service_idx;
    uint32_t type;        /* io_service_open_extended type argument */
    mach_port_name_t owning_task;
    mach_port_name_t async_port;
    uint64_t async_ref[IOKIT_MAX_ASYNC_REF];
    mach_port_name_t notify_port;
    uint32_t notify_type;
    uint64_t notify_ref;
    uint32_t refcount;
    uint64_t memory_addr;
    uint64_t memory_size;
} iokit_connection;

/* One matching-services iterator. matches holds service table
 * indices in registry order; cursor is the next index to return. */
typedef struct {
    int in_use;
    mach_port_name_t port;
    uint32_t matches[IOKIT_MAX_SERVICES];
    uint32_t count;
    uint32_t cursor;
    uint32_t refcount;
} iokit_iterator;

/* Required ipc.c addition (see the file header): create one receive
 * port of kind PORT_KOBJECT_IOKIT in the caller task. Declared here
 * so iokit.c can call it; provided by the ipc.c patch on target,
 * stubbed by the host test (returning incrementing fake names). */
extern mach_port_name_t ipc_iokit_port_create(void);

/* Initialise the registry (idempotent) and register the root entry
 * (class "IOResources", the registry root). Creates the master port
 * via ipc_iokit_port_create. Returns KERN_SUCCESS or a KERN_* code.
 * Must be called once at libsystem_kernel startup, after ipc_init()
 * and after the ipc.c hook is in place. */
int iokit_init(void);

/* The IOKit master port (0 before iokit_init). This is the port
 * host_get_io_master must return; until that hook exists, clients
 * call this directly. */
mach_port_name_t iokit_master_port(void);

/*
 * Register a service: allocate its kobject port, store the class
 * name and a copy of the OSSerializeBinary property blob, and
 * record the kind. Re-registering an existing class name updates
 * its properties and kind and returns the existing port.
 * Returns KERN_SUCCESS or a KERN_* code (KERN_NO_SPACE if the
 * table is full, KERN_INVALID_ARGUMENT on a bad class name or an
 * oversized property blob). The property blob is validated with
 * osser_decode_auto before it is stored (a malformed blob is
 * rejected with KERN_INVALID_ARGUMENT); an XML blob is re-encoded
 * to binary on the way in, so the stored form is always binary.
 */
int iokit_register_service(const char *class_name, uint32_t kind,
                           const uint8_t *props, uint32_t props_len,
                           mach_port_name_t *out_port);

/*
 * Register a service with a small built-in property list instead of
 * a blob: keys[i] maps to vals[i] (all numbers). The blob is built
 * with the osser writer ({keys[0]: vals[0], ...}) plus the IOClass
 * string, so the service answers registry queries the same way as
 * a blob-registered one. n must be <= 16.
 */
int iokit_register_service_simple(const char *class_name, uint32_t kind,
                                  const char *const *keys,
                                  const uint64_t *vals, uint32_t n,
                                  mach_port_name_t *out_port);

/* Look up a registered service's port name (in-process use, e.g.
 * tests or other lsk code). Returns MACH_PORT_NULL if unknown. */
mach_port_name_t iokit_lookup_service_port(const char *class_name);

/* Replace one numeric property of a service, by key, in its stored
 * property blob (decode, rewrite the value, re-encode). Used by
 * IOSurfaceRoot to keep its surface count current. Returns
 * KERN_SUCCESS, KERN_INVALID_NAME (no such service) or
 * KERN_INVALID_ARGUMENT (no such key, or the new blob would not
 * fit). */
int iokit_service_set_property_u64(mach_port_name_t svc_port,
                                   const char *key, uint64_t val);

/* Record the memory range io_connect_map_memory returns for a
 * connection (generic services). IOSurface connections use
 * iosurface_map_memory instead, which resolves the surface id.
 * Returns KERN_SUCCESS or KERN_INVALID_NAME. */
int iokit_connection_set_memory(mach_port_name_t conn_port,
                                uint64_t addr, uint64_t size);

/* Connection introspection for the io_connect_method dispatch:
 * the service kind and service port of a connection. Returns
 * KERN_SUCCESS or KERN_INVALID_NAME. */
int iokit_connection_service(mach_port_name_t conn_port,
                             uint32_t *kind_out,
                             mach_port_name_t *svc_port_out);

/*
 * Kobject entry point, same shape as svchost_process_msg /
 * launchd_process_msg. Called by the ipc.c dispatch branch for
 * PORT_KOBJECT_IOKIT. Parses the incoming MIG request, runs the
 * routine, and builds the MIG reply in reply_buf (header + NDR +
 * RetCode + out parameters, plus port descriptors when the routine
 * returns a port, plus one OOL descriptor when the routine returns
 * an io_buf_ptr_t; msgh_id = request id + 100 [guess], reply on
 * hdr->msgh_local_port with MOVE_SEND_ONCE).
 *
 * Returns 1 if a reply was built (*out_reply_size set, 0 for a
 * one-way message with no reply port) or 0 only if no reply could
 * be built at all (reply_buf too small for even the minimal MIG
 * error reply, 36 bytes). A return of 1 with a reply port always
 * means a reply was built: malformed requests get a MIG reply
 * with an error RetCode, never silence, never a crash. Routines
 * that v1 does not implement (notifications, catalog) answer
 * kIOReturnUnsupported; unknown routine ids answer
 * KERN_INVALID_ARGUMENT. Trailing bytes after the fields a
 * routine expects are tolerated (not treated as malformed): the
 * .defs layouts for a few routines are [guess] and strictness
 * would turn a small layout error into a dropped call.
 */
int iokit_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                      uint8_t *reply_buf, uint32_t max_reply_size,
                      uint32_t *out_reply_size);

/* Trace: true when IOSWINE_SVC_TRACE=1 (same mechanism as bsxpc:
 * lsk_getenv, cached, override with iokit_set_trace). Logs one
 * line per MIG message to fd 2: routine id, destination port, and
 * the io_connect_method selector when present. */
int iokit_trace_enabled(void);
void iokit_set_trace(int on);

/* ------------------------------------------------------------------ */
/* io_connect_method parsed form.                                      */
/*                                                                     */
/* Built by the 2865/2866 handlers from the MIG request and handed   */
/* to the service's method handler (iosurface_connect_method for      */
/* IOSurfaceRoot). struct_in points into the request buffer (no      */
/* copy); struct_out points at a static buffer owned by iokit.c      */
/* (single-threaded under ipc_lock, so no aliasing problem).          */
/* ool_in is a raw address in the caller's task; in-process that      */
/* address is directly readable. ool_out_addr/ool_out_size are       */
/* filled by the handler when it returns OOL output; the default is  */
/* the request's ool_output address with size 0 (nothing returned).  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t selector;
    uint64_t scalar_in[IOKIT_MAX_SCALARS];
    uint32_t scalar_in_cnt;
    const uint8_t *struct_in;
    uint32_t struct_in_len;
    uint64_t ool_in;
    uint32_t ool_in_size;
    uint32_t scalar_out_cap;
    uint32_t struct_out_cap;
    uint64_t ool_out;          /* request: client OOL output address */
    uint32_t ool_out_size_cap; /* request: client OOL output capacity */
    /* Outputs, filled by the method handler. */
    uint32_t retcode;
    uint64_t scalar_out[IOKIT_MAX_SCALARS];
    uint32_t scalar_out_cnt;
    uint8_t *struct_out;       /* static buffer in iokit.c */
    uint32_t struct_out_len;
    uint64_t ool_out_addr;     /* handler-set OOL output address */
    uint32_t ool_out_size;     /* handler-set OOL output size */
} iokit_method_call;
