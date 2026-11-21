/* xpcwire.h - freestanding libxpc wire-format library for ioswine.
 *
 * Implements the libxpc serialization format used inside Mach messages:
 * the "CPX@" magic plus version header, typed values, dictionaries and
 * arrays, and the Mach message envelope that carries port descriptors
 * and out-of-line bodies. No libc: only <stdint.h> and <stddef.h> are
 * used, all helpers are xw_ prefixed so this links alongside launchd.c
 * (ld_ helpers) without symbol clashes, and no multi-byte value is ever
 * read or written through an unaligned pointer.
 *
 * Wire format summary:
 *   body := magic u32 (0x40585043, bytes "CPX@") | version u32 (5) | value
 *   value := type u32 | payload, where the payload shape depends on type
 *   dictionary := 0xf000 | size u32 | count u32 | (key, value)*
 *   array      := 0xe000 | size u32 | count u32 | value*
 *   size is 4 + the sum of the entry/item sizes, all values 4-byte
 *   aligned, keys NUL terminated and padded to 4, string and data
 *   payloads length prefixed and padded to 4. Port carrying values
 *   (mach_send, mach_recv, mach_send_once, fd, shmem, connection,
 *   endpoint) carry no body payload beyond the type word (fd and shmem
 *   carry a u32 length where noted in the doc) and each consumes one
 *   port descriptor from the Mach message, in wire order.
 *
 * Error codes: every function returns XW_OK (0) or a negative XW_ERR_*.
 * The builder keeps a sticky error: after the first failure all further
 * builder calls are no-ops until the caller checks xw_builder_error and
 * discards the buffer. The reader never reads past the buffer it was
 * given and rejects unknown types instead of guessing.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Error codes */
#define XW_OK                0
#define XW_ERR_NOSPACE      -1  /* builder buffer too small */
#define XW_ERR_BAD_MAGIC    -2  /* magic bytes are not "CPX@" */
#define XW_ERR_BAD_VERSION  -3  /* version is not 5 */
#define XW_ERR_TRUNCATED    -4  /* buffer ends inside a value */
#define XW_ERR_BAD_TYPE     -5  /* unknown type code, or getter/type mismatch */
#define XW_ERR_NOT_FOUND    -6  /* dictionary key not present */
#define XW_ERR_OVERFLOW     -7  /* nesting too deep, or too many descriptors */
#define XW_ERR_BAD_ENVELOPE -8  /* Mach envelope is malformed */

/* Wire constants */
#define XW_MAGIC_CPX   0x40585043U  /* bytes "CPX@" on the wire */
#define XW_VERSION     5U

/* XPC value type codes. Codes marked [unverified] in xpc-wire.md could
 * not be confirmed against a live source; the reader accepts them and
 * the builder emits them, but a real libxpc peer may use another code.
 */
#define XW_TYPE_NULL           0x00001000U
#define XW_TYPE_BOOL           0x00002000U
#define XW_TYPE_INT64          0x00003000U
#define XW_TYPE_UINT64         0x00004000U
#define XW_TYPE_DOUBLE         0x00005000U
#define XW_TYPE_DATE           0x00007000U  /* ns since Unix epoch, int64 */
#define XW_TYPE_DATA           0x00008000U
#define XW_TYPE_STRING         0x00009000U
#define XW_TYPE_UUID           0x0000a000U
#define XW_TYPE_FD             0x0000b000U
#define XW_TYPE_SHMEM          0x0000c000U  /* [unverified] */
#define XW_TYPE_MACH_SEND      0x0000d000U
#define XW_TYPE_ARRAY          0x0000e000U
#define XW_TYPE_DICTIONARY     0x0000f000U
#define XW_TYPE_ENDPOINT       0x00010000U  /* [unverified] */
#define XW_TYPE_MACH_SEND_ONCE 0x00012000U  /* [unverified] */
#define XW_TYPE_CONNECTION     0x00013000U
#define XW_TYPE_MACH_RECV      0x00015000U
#define XW_TYPE_FILE_TRANSFER  0x0001a000U  /* RSD transport only, not supported here */

/* Mach message constants used by the envelope layer. These mirror the
 * values in libsystem_kernel/src/ipc.h so this header stays usable
 * standalone (host test) without including ipc.h.
 */
#define XW_MSGH_BITS_COMPLEX      0x80000000U
#define XW_MSGH_ID_XPC            0x10000000U  /* xpc_connection message */
#define XW_MSGH_ID_XPC_REPLY      0x20000000U  /* xpc_pipe_routine reply */
#define XW_MSG_TYPE_MOVE_RECEIVE  16U
#define XW_MSG_TYPE_MOVE_SEND     17U
#define XW_MSG_TYPE_MOVE_SEND_ONCE 18U
#define XW_MSG_TYPE_COPY_SEND     19U
#define XW_MSG_TYPE_MAKE_SEND     20U
#define XW_MSG_TYPE_MAKE_SEND_ONCE 21U
#define XW_DESC_PORT              0U  /* mach_msg_port_descriptor_t, 12 bytes */
#define XW_DESC_OOL               1U  /* mach_msg_ool_descriptor_t, 16 bytes */
#define XW_DESC_OOL_PORTS         2U  /* unsupported by the envelope parser */
#define XW_DESC_OOL_VOLATILE      3U  /* like OOL, 16 bytes */
#define XW_DESC_GUARDED_PORT      4U  /* unsupported by the envelope parser */

#define XW_MAX_DEPTH 16  /* max container nesting, builder and reader */
#define XW_MAX_DESCS 16  /* max descriptors per message / builder */
#define XW_NO_DESC   0xffffffffU  /* no descriptor consumed by this value */

/* One Mach message descriptor, in parsed form.
 *   dtype:       descriptor type byte (XW_DESC_*).
 *   disposition: port descriptors only; the Mach disposition byte.
 *   obj:         port descriptors: the port name in the low 32 bits for a
 *                received message, or a smuggled 64-bit pointer (an
 *                ipc_port* in the lsk) for a locally built message, in
 *                descriptor bytes 0..7. OOL descriptors: the region
 *                address.
 *   size:        OOL descriptors: byte size. Builder side: region length
 *                for OOL data and shmem values, 0 for plain port values.
 */
typedef struct {
    uint32_t dtype;
    uint32_t disposition;
    uint64_t obj;
    uint32_t size;
} xw_desc;

/* ------------------------------------------------------------------ */
/* Builder: caller supplied buffer, never written past cap.            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;   /* bytes written so far, including the 8-byte prefix */
    int err;        /* sticky error, XW_OK while healthy */
    struct {
        uint32_t size_off;   /* offset of the container size word */
        uint32_t count_off;  /* offset of the container count word */
        uint32_t count;      /* entries/items added so far */
        uint32_t kind;       /* XW_TYPE_DICTIONARY or XW_TYPE_ARRAY */
    } stack[XW_MAX_DEPTH];
    uint32_t depth;
    /* Descriptors recorded by port and OOL-data values, in wire order.
     * The envelope layer turns these into real Mach message descriptors;
     * obj/disposition are caller supplied (an ipc_port* plus the
     * disposition to send it with, e.g. COPY_SEND for a mach_send).
     */
    xw_desc descs[XW_MAX_DESCS];
    uint32_t ndescs;
} xw_builder;

/* Write the 8-byte magic + version prefix. buf must stay alive while
 * the builder is used. Returns 0 or a negative XW_ERR_*.
 */
int xw_builder_init(xw_builder *b, uint8_t *buf, uint32_t cap);

/* Bytes written so far (0 if init failed), and the sticky error. */
uint32_t xw_builder_len(const xw_builder *b);
int xw_builder_error(const xw_builder *b);

/* Containers. dict_begin/array_begin start a container as the root
 * value or as an array element; dict_add_dict_begin/dict_add_array_begin
 * start one as a dictionary value under key. dict_end/array_end patch
 * the size and count words. All return 0 or a negative XW_ERR_*.
 */
int xw_dict_begin(xw_builder *b);
int xw_dict_end(xw_builder *b);
int xw_array_begin(xw_builder *b);
int xw_array_end(xw_builder *b);
int xw_dict_add_dict_begin(xw_builder *b, const char *key);
int xw_dict_add_array_begin(xw_builder *b, const char *key);

/* Unkeyed adders: append a value to the innermost array. obj is the
 * port object (an ipc_port* in the lsk, or a port name in tests) and
 * disp is the Mach disposition to send it with. data_ool records an
 * OOL data value: the body carries type + length only, and one port
 * descriptor (a memory entry port) is recorded for the envelope layer.
 * date is int64 nanoseconds since the Unix epoch (1970-01-01).
 */
int xw_add_null(xw_builder *b);
int xw_add_bool(xw_builder *b, int v);
int xw_add_int64(xw_builder *b, int64_t v);
int xw_add_uint64(xw_builder *b, uint64_t v);
int xw_add_double(xw_builder *b, double v);
int xw_add_string(xw_builder *b, const char *s);
int xw_add_data(xw_builder *b, const void *p, uint32_t len);
int xw_add_data_ool(xw_builder *b, uint32_t len, uint64_t obj, uint32_t disp);
int xw_add_uuid(xw_builder *b, const uint8_t uuid[16]);
int xw_add_date(xw_builder *b, int64_t ns_since_unix_epoch);
int xw_add_fd(xw_builder *b, uint64_t obj, uint32_t disp);
int xw_add_mach_send(xw_builder *b, uint64_t obj, uint32_t disp);
int xw_add_mach_recv(xw_builder *b, uint64_t obj, uint32_t disp);
int xw_add_mach_send_once(xw_builder *b, uint64_t obj, uint32_t disp);
int xw_add_shmem(xw_builder *b, uint64_t obj, uint32_t disp, uint32_t len);

/* Keyed adders: append a key/value pair to the innermost dictionary. */
int xw_dict_add_null(xw_builder *b, const char *key);
int xw_dict_add_bool(xw_builder *b, const char *key, int v);
int xw_dict_add_int64(xw_builder *b, const char *key, int64_t v);
int xw_dict_add_uint64(xw_builder *b, const char *key, uint64_t v);
int xw_dict_add_double(xw_builder *b, const char *key, double v);
int xw_dict_add_string(xw_builder *b, const char *key, const char *s);
int xw_dict_add_data(xw_builder *b, const char *key, const void *p, uint32_t len);
int xw_dict_add_data_ool(xw_builder *b, const char *key, uint32_t len,
                         uint64_t obj, uint32_t disp);
int xw_dict_add_uuid(xw_builder *b, const char *key, const uint8_t uuid[16]);
int xw_dict_add_date(xw_builder *b, const char *key, int64_t ns_since_unix_epoch);
int xw_dict_add_fd(xw_builder *b, const char *key, uint64_t obj, uint32_t disp);
int xw_dict_add_mach_send(xw_builder *b, const char *key, uint64_t obj, uint32_t disp);
int xw_dict_add_mach_recv(xw_builder *b, const char *key, uint64_t obj, uint32_t disp);
int xw_dict_add_mach_send_once(xw_builder *b, const char *key, uint64_t obj, uint32_t disp);
int xw_dict_add_shmem(xw_builder *b, const char *key, uint64_t obj, uint32_t disp,
                      uint32_t len);

/* ------------------------------------------------------------------ */
/* Reader: zero-copy, every read bounds checked.                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *buf;  /* XPC body (magic .. end), not owned */
    uint32_t len;
    const xw_desc *descs;  /* envelope descriptors, in wire order */
    uint32_t ndescs;
} xw_reader;

/* One parsed value. Scalar fields are only valid for their type; ptr
 * points into the reader buffer (string bytes including the NUL,
 * inline data bytes, uuid bytes) and is NULL for OOL and port values;
 * desc is the envelope descriptor index for port and OOL values and
 * XW_NO_DESC otherwise; body/body_len describe a container's content
 * (starting at its count word); desc_cursor is the descriptor index
 * the container's first child would consume.
 */
typedef struct {
    uint32_t type;
    int64_t i64;         /* int64, date (ns since Unix epoch) */
    uint64_t u64;        /* uint64 */
    double dbl;          /* double */
    int boolean;         /* bool */
    const uint8_t *ptr;  /* string / data / uuid inline bytes */
    uint32_t len;        /* string len incl NUL, data len, uuid 16, OOL/shmem len */
    uint32_t desc;       /* descriptor index, or XW_NO_DESC */
    const uint8_t *body; /* container content start (count word) */
    uint32_t body_len;   /* container content length (size word value) */
    const xw_desc *descs;
    uint32_t ndescs;
    uint32_t desc_cursor;
    uint32_t depth;      /* nesting level of this value (root is 0) */
} xw_value;

/* Validate magic, version and root, then fully validate the whole
 * message (every value, size, count and descriptor use). descs may be
 * NULL with ndescs 0 when the message carries no ports. Returns 0 or
 * a negative XW_ERR_*.
 */
int xw_reader_init(xw_reader *r, const uint8_t *buf, uint32_t len,
                   const xw_desc *descs, uint32_t ndescs);

/* The root value (dictionary, array, or endpoint). */
int xw_reader_root(xw_reader *r, xw_value *out);

/* Dictionary lookup by key. The _v forms take a dictionary value
 * (from xw_reader_root, xw_dict_find, xw_array_at, or the iterator)
 * so nested containers work the same way. Returns XW_ERR_NOT_FOUND
 * when the key is absent, XW_ERR_BAD_TYPE when the value is not a
 * dictionary. First matching key wins.
 */
int xw_dict_find(xw_reader *r, const char *key, xw_value *out);
int xw_dict_find_v(const xw_value *dict, const char *key, xw_value *out);

/* Dictionary iterator. init on the root (xw_dict_iter_init) or on a
 * nested dictionary value (xw_dict_iter_init_v). xw_dict_next returns
 * 1 with key_out pointing at the NUL terminated key inside the buffer
 * and out holding the value, 0 when the entries are exhausted, or a
 * negative XW_ERR_* on a malformed entry.
 */
typedef struct {
    const uint8_t *pos;
    const uint8_t *end;
    uint32_t remaining;
    uint32_t next_desc;
    const xw_desc *descs;
    uint32_t ndescs;
    uint32_t depth;      /* nesting level of the values it yields */
} xw_dict_iter;

int xw_dict_iter_init(xw_reader *r, xw_dict_iter *it);
int xw_dict_iter_init_v(const xw_value *dict, xw_dict_iter *it);
int xw_dict_next(xw_dict_iter *it, const char **key_out, xw_value *out);

/* Array access. The _v forms take an array value for nested arrays. */
int xw_array_len(xw_reader *r, uint32_t *out);
int xw_array_len_v(const xw_value *arr, uint32_t *out);
int xw_array_at(xw_reader *r, uint32_t idx, xw_value *out);
int xw_array_at_v(const xw_value *arr, uint32_t idx, xw_value *out);

/* Typed getters. All strictly check the value type and return 0 or a
 * negative XW_ERR_*. Port getters return the envelope descriptor INDEX
 * (the name lives in the envelope, see xw_desc). xw_as_data returns
 * ptr == NULL and *desc_out set for OOL data, ptr set and
 * *desc_out == XW_NO_DESC for inline data.
 */
int xw_as_int64(const xw_value *v, int64_t *out);
int xw_as_uint64(const xw_value *v, uint64_t *out);
int xw_as_bool(const xw_value *v, int *out);
int xw_as_double(const xw_value *v, double *out);
int xw_as_string(const xw_value *v, const char **out, uint32_t *len_out);
int xw_as_data(const xw_value *v, const uint8_t **ptr_out, uint32_t *len_out,
               uint32_t *desc_out);
int xw_as_uuid(const xw_value *v, const uint8_t **out);
int xw_as_date(const xw_value *v, int64_t *ns_out);
int xw_as_fd(const xw_value *v, uint32_t *desc_out);
int xw_as_mach_send(const xw_value *v, uint32_t *desc_out);
int xw_as_mach_recv(const xw_value *v, uint32_t *desc_out);
int xw_as_mach_send_once(const xw_value *v, uint32_t *desc_out);
int xw_as_shmem(const xw_value *v, uint32_t *desc_out, uint32_t *len_out);
int xw_as_connection(const xw_value *v, uint32_t *desc_out);
int xw_as_endpoint(const xw_value *v, uint32_t *desc_out);

/* ------------------------------------------------------------------ */
/* Mach message envelope.                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t bits;
    uint32_t size;     /* msgh_size, the extent of the message */
    uint32_t remote;   /* msgh_remote_port */
    uint32_t local;    /* msgh_local_port (reply port on a request) */
    uint32_t voucher;  /* msgh_voucher_port */
    uint32_t id;       /* msgh_id */
    xw_desc descs[XW_MAX_DESCS];  /* port descriptors, in wire order */
    uint32_t ndescs;
    const uint8_t *payload;  /* inline XPC body, or OOL body address */
    uint32_t payload_len;
    int body_is_ool;   /* payload came from the first OOL descriptor */
} xw_envelope;

/* Parse a Mach message header plus descriptors. msgh_size is used as
 * the body extent; trailer bytes past msgh_size are ignored. A single
 * leading OOL descriptor means the whole XPC body is out of line
 * (payload then points at the OOL address, which the kernel maps into
 * the receiver, so the address is trusted here). OOL ports and guarded
 * port descriptors are rejected with XW_ERR_BAD_ENVELOPE. Returns 0
 * or a negative XW_ERR_*.
 */
int xw_envelope_parse(const void *msg, uint32_t size, xw_envelope *env);

/* Build an xpc_pipe_routine reply: msgh_id 0x20000000, remote port is
 * the request's local (reply) port sent as MOVE_SEND_ONCE, local and
 * voucher ports NULL. body/body_len is the already serialized XPC
 * reply; descs are the reply port descriptors (obj holds the port name
 * or a smuggled ipc_port*, disposition the send disposition, typically
 * COPY_SEND 0x13). The message is complex only when ndescs > 0.
 * Writes the total message size to *out_len. Returns 0 or a negative
 * XW_ERR_* (XW_ERR_NOSPACE when cap is too small).
 */
int xw_envelope_build_reply(const xw_envelope *req, uint8_t *out, uint32_t cap,
                            const uint8_t *body, uint32_t body_len,
                            const xw_desc *descs, uint32_t ndescs,
                            uint32_t *out_len);
