/* metallib.h - freestanding C reader for the Apple .metallib container.
 *
 * Format (grounded in two public sources that agree on the 88-byte header:
 * zhuowei, "Compile Metal shader Bitcode to x86 and ARM assembly",
 * worthdoingbadly.com/metalbitcode, 2015; and YuAo, MetalLibraryArchive,
 * whose layout was recovered from disassembling Metal.framework's
 * MTLLibraryDataWithArchive::parseArchiveSync):
 *
 *   Header, 88 bytes, all little-endian:
 *     0..3   FourCC "MTLB" (0x424c544d)
 *     4..5   UInt16 target platform (iOS 0x0001, macOS 0x8001)
 *     6..9   (UInt16, UInt16) metallib version (major, minor)
 *     10     UInt8 library type (0 executable, 1 Core Image,
 *            2 dynamic, 3 symbol companion)
 *     11     UInt8 target OS (0x81 macOS, 0x82 iOS, 0x83 tvOS,
 *            0x84 watchOS, 0x85 bridgeOS, 0x86 macCatalyst,
 *            0x87 iOS Simulator, 0x88 tvOS Simulator, 0x89 watchOS Sim)
 *     12..15 (UInt16, UInt16) target OS version (major, minor)
 *     16..23 UInt64 size of the metallib file
 *     24..39 (UInt64, UInt64) function list offset and size
 *     40..55 (UInt64, UInt64) public metadata offset and size
 *     56..71 (UInt64, UInt64) private metadata offset and size
 *     72..87 (UInt64, UInt64) bitcode section offset and size
 *
 *   Function list: UInt32 entry count, then one tag group per function.
 *   Tag group: UInt32 size, then tags. The size is the span from the
 *   size field itself to the next group's size field (it includes its
 *   own 4 bytes), for FUNCTION groups at least: the 2015 file's first
 *   group declares 0x82 and spans exactly 0x5c..0xde. The header's
 *   function-list size is NOT a bound on the groups: in that same file
 *   it is 0x106, which lands on the last group's ENDT, not past it
 *   (the count and the final ENDT cancel in the arithmetic), and the
 *   YuAo parser never uses it to advance either. This reader therefore
 *   scans groups to ENDT and bounds them by the file extent only,
 *   exactly like that parser; it does not trust the header's
 *   function-list size as a group bound.
 *   Tag: FourCC code, UInt16 size, payload bytes. A group ends with an
 *   ENDT tag, which is a bare 4-byte code with NO size field and NO
 *   payload. (That last point is not spelled out in the YuAo doc, whose
 *   tag table implies every tag has a UInt16 size; the 2015 hex dump
 *   proves the bare form: the first group's declared size 0x82 spans
 *   exactly 0x5c..0xde, which only works if ENDT is 4 bytes. A reader
 *   that consumed a 2-byte size after ENDT would drift 2 bytes per
 *   group and misparse every group after the first.)
 *
 *   Function information tags:
 *     NAME  NUL-terminated C string, function name (the tag size
 *           includes the NUL)
 *     MDSZ  UInt64, size of this function's bitcode blob. When absent,
 *           the size falls back to the next function's bitcode offset
 *           minus this one, or the rest of the bitcode section for the
 *           last function (YuAo parser behaviour).
 *     TYPE  UInt8, 0 vertex, 1 fragment, 2 kernel, 3 unqualified,
 *           4 visible, 5 extern, 6 intersection
 *     HASH  32 bytes, SHA-256 of the bitcode blob
 *     OFFT  (UInt64, UInt64, UInt64), offsets of this function's data
 *           inside the public metadata section, the private metadata
 *           section, and the bitcode section (each relative to its
 *           own section start)
 *     SOFF  UInt64, offset of the source archive in the embedded
 *           source code section
 *     VERS  (UInt16 x4) air.major, air.minor, language.major,
 *           language.minor
 *     LAYR  UInt8, render_target_array_index type (layered rendering)
 *     TESS  UInt8, controlPointCount << 2 | patchType (1 triangle,
 *           2 quad)
 *     ENDT  end of the tag group
 *
 *   Header extension: present when
 *     function_list_offset + function_list_size + 4 != public_metadata_offset
 *   and holds HDYN/VLST/ILST/HSRD/HSRC/UUID/ENDT tags. This reader does
 *   not parse it: the function list is located from the header offsets,
 *   so the extension never affects function listing or bitcode
 *   extraction. tools/metallib_scan.py reports its tags.
 *
 * The reader never allocates and never copies the file: names and
 * bitcode blobs are pointers into the caller's buffer, which must stay
 * alive while the library struct is used. Every offset is bounds
 * checked against the file size recorded in the header.
 *
 * No libc. Freestanding: -Wall -Wextra -Werror clean with the NDK
 * aarch64 clang -ffreestanding.
 */
#ifndef IOSWINE_METALLIB_H
#define IOSWINE_METALLIB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ML_HEADER_SIZE 88u
#define ML_MAGIC 0x424c544du /* "MTLB" */

/* Return codes: 0 is success, anything else is negative. */
#define ML_OK 0
#define ML_ERR_TOO_SMALL -1    /* buffer smaller than the 88-byte header */
#define ML_ERR_BAD_MAGIC -2    /* first four bytes are not "MTLB" */
#define ML_ERR_BAD_FILE_SIZE -3 /* header file size disagrees with the buffer */
#define ML_ERR_BAD_SECTION -4  /* a section offset/size runs past the file */
#define ML_ERR_TRUNCATED -5   /* function list or a tag group is cut short */
#define ML_ERR_TOO_MANY -6    /* more functions than ML_MAX_FUNCTIONS */
#define ML_ERR_BAD_TAG -7     /* a tag runs past its group, or has a bad size */
#define ML_ERR_NO_ENDT -8     /* a tag group does not end with ENDT */
#define ML_ERR_NO_NAME -9     /* a function entry has no usable NAME tag */
#define ML_ERR_BAD_BITCODE -10 /* bitcode offset/size runs past the section */
#define ML_ERR_BAD_INDEX -11   /* function index out of range */

/* Maximum functions kept in one ml_library_t. The struct is ~20 KiB at
 * 256 entries; callers allocate it statically, never on a small stack. */
#ifndef ML_MAX_FUNCTIONS
#define ML_MAX_FUNCTIONS 256
#endif

/* Target platforms (header bytes 4..5). */
#define ML_PLATFORM_IOS 0x0001u
#define ML_PLATFORM_MACOS 0x8001u

/* Library types (header byte 10). */
#define ML_TYPE_EXECUTABLE 0u
#define ML_TYPE_CORE_IMAGE 1u
#define ML_TYPE_DYNAMIC 2u
#define ML_TYPE_SYMBOL_COMPANION 3u

/* Target operating systems (header byte 11). */
#define ML_OS_UNKNOWN 0x00u
#define ML_OS_MACOS 0x81u
#define ML_OS_IOS 0x82u
#define ML_OS_TVOS 0x83u
#define ML_OS_WATCHOS 0x84u
#define ML_OS_BRIDGEOS 0x85u
#define ML_OS_MACCATALYST 0x86u
#define ML_OS_IOS_SIMULATOR 0x87u
#define ML_OS_TVOS_SIMULATOR 0x88u
#define ML_OS_WATCHOS_SIMULATOR 0x89u

/* Function types (TYPE tag payload). */
#define ML_FUNC_VERTEX 0u
#define ML_FUNC_FRAGMENT 1u
#define ML_FUNC_KERNEL 2u
#define ML_FUNC_UNQUALIFIED 3u
#define ML_FUNC_VISIBLE 4u
#define ML_FUNC_EXTERN 5u
#define ML_FUNC_INTERSECTION 6u

/* Bitcode blob magics, for ml_function bitcode_ok. */
#define ML_BITCODE_WRAPPER_MAGIC 0x0b17c0deu /* .air / metallib blob */
#define ML_BITCODE_MAGIC 0xdec04342u         /* "BC\xc0\xde" raw stream */

typedef struct {
    uint16_t platform;
    uint16_t version_major;
    uint16_t version_minor;
    uint8_t lib_type;
    uint8_t target_os;
    uint16_t os_major;
    uint16_t os_minor;
    uint64_t file_size;
    uint64_t func_list_off;
    uint64_t func_list_size;
    uint64_t pub_meta_off;
    uint64_t pub_meta_size;
    uint64_t priv_meta_off;
    uint64_t priv_meta_size;
    uint64_t bitcode_off;
    uint64_t bitcode_size;
} ml_header_t;

typedef struct {
    const char *name;      /* NUL-terminated, points into the file buffer */
    uint32_t name_len;     /* bytes before the NUL, not counting it */
    uint8_t func_type;     /* ML_FUNC_*; 0xff when no TYPE tag was present */
    uint8_t has_hash;      /* HASH tag present */
    uint8_t has_offt;      /* OFFT tag present */
    uint8_t has_mdsz;      /* MDSZ tag present */
    uint8_t bitcode_ok;    /* blob starts with a known bitcode magic */
    uint8_t layr;          /* LAYR tag, 0 when absent */
    uint8_t tess;          /* TESS tag, 0 when absent */
    uint16_t air_major;    /* VERS tag, 0 when absent */
    uint16_t air_minor;
    uint16_t lang_major;
    uint16_t lang_minor;
    uint64_t bitcode_size; /* MDSZ tag, 0 when absent */
    uint64_t bitcode_off;  /* absolute file offset of the blob */
    uint64_t soff;         /* SOFF tag, 0 when absent */
    uint64_t offt[3];      /* OFFT tag: public, private, bitcode offsets */
    uint8_t hash[32];      /* HASH tag payload */
} ml_function_t;

typedef struct {
    const uint8_t *data; /* caller's buffer, not owned */
    uint32_t size;       /* bytes available in data */
    ml_header_t header;
    uint32_t count;
    ml_function_t funcs[ML_MAX_FUNCTIONS];
} ml_library_t;

/* Parse the header and the whole function list. On success the library
 * struct holds the function table; on failure it holds nothing usable
 * (count is 0) and the return value names the problem. */
int ml_open(const uint8_t *data, uint32_t size, ml_library_t *lib);

/* Return the embedded bitcode blob for function index: a pointer into the
 * original buffer and its size. Fails with ML_ERR_BAD_INDEX or
 * ML_ERR_BAD_BITCODE. The blob itself starts with the LLVM bitcode
 * wrapper magic (0x0b17c0de); see bitcode.h to walk it. */
int ml_function_bitcode(const ml_library_t *lib, uint32_t index,
                        const uint8_t **out, uint32_t *out_size);

/* Linear search by exact name. Returns the function index, or -1. */
int ml_find(const ml_library_t *lib, const char *name);

/* Human-readable names for the header fields. Unknown values yield
 * "unknown". Pure if/else chains, no tables of function pointers. */
const char *ml_platform_name(uint16_t platform);
const char *ml_lib_type_name(uint8_t lib_type);
const char *ml_target_os_name(uint8_t target_os);
const char *ml_func_type_name(uint8_t func_type);

/* One-line description of an ml_* error code. */
const char *ml_error_string(int err);

#ifdef __cplusplus
}
#endif

#endif /* IOSWINE_METALLIB_H */
