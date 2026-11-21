/*
 * iosurface.h: IOSurfaceRoot user-client emulation for ioswine
 *
 *
 * Registers the "IOSurfaceRoot" IOKit service (opened with type 0)
 * and implements its external methods (io_connect_method
 * selectors) on top of iokit.{c,h}. Surfaces are memfd-backed on
 * the target so the Android presenter can later receive the fd
 * over the @ioswine-surface socket (surface/PROTOCOL.md); on the
 * host the test provides the platform hook below.
 *
 * Sources and [guess] list. The [src:]
 * tags point at public sources.
 *
 * Sourced selector numbers (IOSurfaceRootUserClient external
 * methods, iOS 14-16 era; the table may have shifted by iOS 26,
 * so the first device trace must confirm them):
 *   0  s_create_surface          [src: jsherman212 IOMFB writeup]
 *   1  s_release_surface         [src: same]
 *   9  s_set_value               [src: same]
 *   10 s_get_value               [src: same]
 *   13 get_limits                [src: same]
 *   16 get_surface_use_count     [src: Project Zero, Aug 2019]
 *   17 s_set_surface_notify      [src: exploit-db CVE-2016-7612 writeup]
 *   34 s_lookup_surface_from_port [src: jsherman212 IOMFB writeup]
 *   35 create_port_from_surface  [src: same]
 * All other selectors are UNSOURCED. They are implemented as
 * named placeholder constants in the 36+ range (above the highest
 * sourced selector, 35, so they cannot collide with the unknown
 * real numbers in 2-8, 11-12, 14-15, 18-33), each marked
 * [guess: placeholder]. Renumbering one is a one-line #define
 * change in this file; the dispatch in iosurface.c is an if-chain
 * over these defines. Do not hard-code a number for an unsourced
 * selector anywhere else.
 *
 * Memory path decision: IOConnectMapMemory MIG 2863 vs a
 * memory-entry port in the create_surface output struct.
 * The choice is made from the available sources:
 *   - IOConnectMapMemory "asks the UserClient for shared memory
 *     via clientMemoryForType" [src: Ian Beer, SyScan15 slides].
 *     IOSurfaceRootUserClient overrides clientMemoryForType, so
 *     the kernel-side hook exists precisely to hand out per-type
 *     memory, and the only per-surface integer the framework has
 *     at map time is the surface id.
 *   - The create output struct (0xBC8 bytes, s_create_surface
 *     checkStructureOutputSize) is the size the framework offers
 *     for the create reply; its first field is the surface id
 *     [guess] and the rest carries the surface properties.
 * Candidate A is therefore implemented: IOConnectMapMemory (2863)
 * with memory_type == surface id (that mapping is [guess]), and
 * the create reply returns the fixed-size output struct with the
 * surface id at offset 0, the alloc size at offset 4, and the
 * surface properties as an OSSerializeBinary blob at offset 8,
 * zero-filled elsewhere (whole layout [guess]). Candidate B (a
 * memory-entry port inside the create output struct) is NOT
 * implemented; if the first device trace shows the framework
 * never calls 2863 after create, switch to B: put the memfd's
 * memory-entry port descriptor at a fixed offset of the create
 * output struct and have the framework map it. IOSURFACE_MAP_VIA_
 * CONNECT below makes the choice visible in one place.
 *
 * Create input forms: the create struct input is an XML plist
 * dict, or a binary plist (auto-detected, same as
 * OSUnserializeXML). Newer iOS also has
 * a fast-create path taking a binary IOSurfaceFastCreateArgs
 * struct and returning an IOSurfaceLockResult (size 0xf60)
 * [src: CVE-2021-30955 gist]; that struct's layout is not pinned
 * down by any source found here, so v1 does NOT implement it: a
 * struct input that is neither XML nor binary plist returns
 * kIOReturnBadArgument, visibly, so the trace shows the attempt.
 * The IOSurfaceClass key in the create dict is accepted and the
 * override is ignored (default IOSurface only).
 *
 * Surface-id scope: in the real kernel, surface ids are
 * per-client indices into the user client's surface-client array
 * [src: jsherman212 IOMFB writeup]. v1 uses ONE global table, so
 * ids are valid on any connection of the process. The framework
 * keeps one shared connection per process, so this is
 * behaviourally equivalent for the normal path; per-client
 * indexing is a known difference to revisit if a multi-connection
 * lookup fails.
 *
 * Row stride: the kernel aligns bytesPerRow; the rule is unknown
 * [guess: 16 or 64 bytes]. v1 aligns up to 64 bytes and returns
 * the aligned value self-consistently, and uses it for the buffer
 * size. Property keys are the public IOSurfaceAPI.h keys
 * [src: IOSurfaceAPI.h, MacOSX10.8 SDK mirror]: Width, Height,
 * BytesPerRow, BytesPerElement (default 1), ElementWidth/Height
 * (default 1), Offset (default 0), PlaneInfo (array of dicts),
 * AllocSize (defaults to Height*BytesPerRow), CacheMode (default
 * kIOMapDefaultCache), IsGlobal (deprecated iOS 9.0),
 * PixelFormat (32-bit OSType). 1080x2400 BGRA worked numbers:
 * width 1080, height 2400, bytesPerElement 4, bytesPerRow
 * 1080*4 = 4320 before alignment, pixelFormat 0x41524742
 * ("BGRA" in memory order, kCVPixelFormatType_32BGRA),
 * allocSize 1080*4320 = 10368000 before alignment. MemoryRegion
 * "PurpleGfxMem" and BufferTileMode are accepted and treated as
 * ordinary backing store [src: Star exploit dict].
 *
 * Lock flags: kIOSurfaceLockReadOnly 0x00000001,
 * kIOSurfaceLockAvoidSync 0x00000002 [src: IOSurfaceAPI.h].
 * Whether IOSurfaceLock/Unlock issue a kernel call at all is
 * unconfirmed [guess]; v1 implements the selectors so the call
 * works if the framework makes it, and the seed exchange is
 * implemented as documented below.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "iokit.h"

/* Which mapping path the create path implements. 1 = Candidate A
 * (IOConnectMapMemory, memory_type == surface id). 0 would mean
 * Candidate B (memory-entry port in the create output struct).
 * See the header comment for the evidence and trade-offs. */
#define IOSURFACE_MAP_VIA_CONNECT 1

/* Limits. */
#define IOSURFACE_MAX_SURFACES   64
#define IOSURFACE_MAX_PLANES     3
#define IOSURFACE_MAX_PROPS      16
#define IOSURFACE_PROP_KEY_MAX   64
#define IOSURFACE_PROP_STR_MAX   128
#define IOSURFACE_PROP_DATA_MAX  128
#define IOSURFACE_CREATE_OUT_SIZE 0xBC8U  /* 3016, s_create_surface
                                             checkStructureOutputSize
                                             [src: secfault writeup] */
#define IOSURFACE_ROW_ALIGN      64U      /* [guess] */

/* IOSurfaceRootUserClient external method selectors. Sourced
 * numbers carry [src:]; the rest are placeholders, see the header
 * comment. Dispatch in iosurface.c is an if-chain over these. */
#define IOSURFACE_SEL_CREATE              0U  /* [src] s_create_surface */
#define IOSURFACE_SEL_RELEASE             1U  /* [src] s_release_surface */
/* 2-8: unknown selectors, not implemented. */
#define IOSURFACE_SEL_SET_VALUE           9U  /* [src] s_set_value */
#define IOSURFACE_SEL_GET_VALUE           10U /* [src] s_get_value */
/* 11-12: unknown selectors, not implemented. */
#define IOSURFACE_SEL_GET_LIMITS          13U /* [src] get_limits */
/* 14-15: unknown selectors, not implemented. */
#define IOSURFACE_SEL_GET_USE_COUNT       16U /* [src] get_surface_use_count */
#define IOSURFACE_SEL_SET_NOTIFY          17U /* [src] s_set_surface_notify */
/* 18-33: unknown selectors, not implemented. */
#define IOSURFACE_SEL_LOOKUP_FROM_PORT    34U /* [src] s_lookup_surface_from_port */
#define IOSURFACE_SEL_CREATE_PORT         35U /* [src] create_port_from_surface */
/* Placeholders below: [guess: placeholder] values to renumber from
 * the first device trace. One #define each. */
#define IOSURFACE_SEL_LOCK                36U /* [guess: placeholder] */
#define IOSURFACE_SEL_UNLOCK              37U /* [guess: placeholder] */
#define IOSURFACE_SEL_LOOKUP              38U /* [guess: placeholder] lookup by id */
#define IOSURFACE_SEL_GET_SEED            39U /* [guess: placeholder] */
#define IOSURFACE_SEL_GET_PLANE_INFO      40U /* [guess: placeholder] */
#define IOSURFACE_SEL_INCREMENT_USE_COUNT 41U /* [guess: placeholder] */
#define IOSURFACE_SEL_DECREMENT_USE_COUNT 42U /* [guess: placeholder] */
#define IOSURFACE_SEL_PURGE               43U /* [guess: placeholder] */

/* Pixel formats (OSType, first character in the low byte).
 * BGRA 0x41524742 is kCVPixelFormatType_32BGRA; 420v 0x76303234 is
 * kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange. */
#define IOSURFACE_PIXEL_FORMAT_NONE 0U
#define IOSURFACE_PIXEL_FORMAT_BGRA 0x41524742U
#define IOSURFACE_PIXEL_FORMAT_420V 0x76303234U

/* Cache modes (IOMap cache values; default 0 = kIOMapDefaultCache).
 * The non-default values are [guess: standard IOMap ordering]. */
#define IOSURFACE_CACHE_DEFAULT 0U
#define IOSURFACE_CACHE_COPYBACK 1U
#define IOSURFACE_CACHE_WRITETHROUGH 2U
#define IOSURFACE_CACHE_WRITECOMBINING 3U
#define IOSURFACE_CACHE_NOCACHE 5U

/* Lock flags [src: IOSurfaceAPI.h]. */
#define IOSURFACE_LOCK_READONLY  0x00000001U
#define IOSURFACE_LOCK_AVOIDSYNC 0x00000002U

/* get_limits output values [guess]. */
#define IOSURFACE_LIMIT_MAX_WIDTH  16384U
#define IOSURFACE_LIMIT_MAX_HEIGHT 16384U
#define IOSURFACE_LIMIT_MAX_BYTES_PER_ROW 65536U

/* Per-surface property value kinds. */
#define IOSURFACE_PROP_NUMBER 1U
#define IOSURFACE_PROP_STRING 2U
#define IOSURFACE_PROP_DATA   3U

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint32_t bytes_per_row;
    uint32_t width;
    uint32_t height;
} iosurface_plane;

typedef struct {
    char key[IOSURFACE_PROP_KEY_MAX];
    uint32_t kind; /* IOSURFACE_PROP_* */
    uint64_t num;  /* number value */
    char str[IOSURFACE_PROP_STR_MAX];
    uint8_t data[IOSURFACE_PROP_DATA_MAX];
    uint32_t data_len;
} iosurface_prop;

/* One surface. id 0 means the slot is free. use_count tracks
 * client references (create starts at 1; create_port adds 1 per
 * live port; release decrements). lock_count is the number of
 * outstanding locks; seed increments on every unlock (and is
 * returned by lock/unlock). fd is the memfd on the target (-1 on
 * host); addr is the mapped base; map_size is the mapping length.
 * port is the Mach port name created for this surface by
 * create_port (0 if none). props is the per-surface property
 * table (creation properties reflected in it). Up to 4 ports per
 * surface are tracked (see the create_port notes above). */
typedef struct {
    int in_use;
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;     /* aligned */
    uint32_t pixel_format;
    uint32_t bytes_per_element;
    uint64_t alloc_size;
    uint32_t cache_mode;
    uint32_t plane_count;
    iosurface_plane planes[IOSURFACE_MAX_PLANES];
    uint32_t lock_count;
    uint32_t seed;
    uint32_t use_count;
    int fd;
    void *addr;
    uint64_t map_size;
    mach_port_name_t ports[4];
    uint32_t port_count;
    iosurface_prop props[IOSURFACE_MAX_PROPS];
    uint32_t prop_count;
} iosurface_surface;

/*
 * Register the "IOSurfaceRoot" service (kind
 * IOKIT_SERVICE_KIND_IOSURFACE_ROOT, opened with type 0) and
 * initialise the surface table. Idempotent; calls iokit_init.
 * Returns KERN_SUCCESS or a KERN_* code.
 */
int iosurface_init(void);

/* The IOSurfaceRoot service port (MACH_PORT_NULL before
 * iosurface_init). */
mach_port_name_t iosurface_root_port(void);

/*
 * io_connect_method handler for connections on IOSurfaceRoot.
 * Called by iokit.c's 2865/2866 handlers. Fills call->retcode and
 * the scalar_out / struct_out / ool_out fields of the call.
 * Dispatch is an if-chain over the IOSURFACE_SEL_* defines.
 *
 * Selector input/output shapes (all [guess] unless noted):
 *  0 create: struct_in = properties dict (XML plist, or binary
 *    plist auto-detected); struct_out = IOSURFACE_CREATE_OUT_SIZE
 *    bytes: u32 surface id at offset 0, u64 alloc size at offset
 *    8 (4 bytes zero padding at offset 4), OSSerializeBinary
 *    property blob at offset 16, zero-filled elsewhere. The whole
 *    layout is [guess]. scalar_out[0] = surface id (also in the
 *    struct, for the caller's convenience). The new surface's use
 *    count starts at 1.
 *  1 release: scalar_in[0] = surface id. Returns kIOReturnBusy
 *    while the surface is locked; otherwise decrements the use
 *    count and destroys the surface (unmaps, closes the memfd)
 *    when it reaches 0.
 *  9 set_value: struct_in = 8-byte prefix (surface id as u64 at
 *    offset 0 [guess]) followed by an OSArray [key, value] (XML or
 *    binary, auto-detected). Stores the key/value in the surface's
 *    property table (max IOSURFACE_MAX_PROPS entries; a full table
 *    returns kIOReturnNoResources).
 *  10 get_value: struct_in = surface id as u64 at offset 0,
 *    NUL-terminated key at offset 8 [guess]; struct_out = u32
 *    length at offset 0, OSSerializeBinary blob of the value at
 *    offset 4 [guess]. Unknown key returns kIOReturnNotFound.
 *  13 get_limits: struct_out = 3 u64 (max width, max height, max
 *    bytesPerRow) = 16384, 16384, 65536 [guess].
 *  16 get_surface_use_count: scalar_in[0] = surface id;
 *    scalar_out[0] = use count.
 *  17 set_surface_notify: scalar_in[0] = surface id,
 *    scalar_in[1] = notification type [guess]; records nothing,
 *    returns success (no notification is ever delivered in v1).
 *  34 lookup_from_port: scalar_in[0] = Mach port name;
 *    scalar_out[0] = surface id; struct_out = OSSerializeBinary
 *    property blob of the surface [guess]. Unknown port returns
 *    kIOReturnNotFound. The port is NOT destroyed by the lookup.
 *  35 create_port: scalar_in[0] = surface id; scalar_out[0] =
 *    new Mach port name (send right) for the surface. The live
 *    port adds 1 to the surface's use count until it is deleted;
 *    v1 has no port-deletion hook, so the increment is permanent
 *    for the process lifetime (documented limitation). Up to 4
 *    ports per surface are tracked for lookup; further create_port
 *    calls still return a fresh port and increment the use count,
 *    but the extra port is not recorded for lookup.
 *  36 lock [guess: placeholder]: scalar_in[0] = surface id,
 *    scalar_in[1] = lock flags; scalar_out[0] = seed. Increments
 *    the lock count.
 *  37 unlock [guess: placeholder]: scalar_in[0] = surface id,
 *    scalar_in[1] = lock flags; scalar_out[0] = new seed.
 *    Decrements the lock count and increments the seed.
 *  38 lookup by id [guess: placeholder]: scalar_in[0] = surface
 *    id; scalar_out[0] = surface id (echo, if it exists);
 *    struct_out = OSSerializeBinary property blob. Unknown id
 *    returns kIOReturnNotFound.
 *  39 get_seed [guess: placeholder]: scalar_in[0] = surface id;
 *    scalar_out[0] = seed.
 *  40 get_plane_info [guess: placeholder]: scalar_in[0] =
 *    surface id; scalar_out[0] = plane count; struct_out = one
 *    20-byte record per plane (offset u32, size u32,
 *    bytesPerRow u32, width u32, height u32).
 *  41 increment_use_count [guess: placeholder]: scalar_in[0] =
 *    surface id; scalar_out[0] = new use count.
 *  42 decrement_use_count [guess: placeholder]: scalar_in[0] =
 *    surface id; scalar_out[0] = new use count (never below 0;
 *    the surface is NOT destroyed by this call).
 *  43 purge [guess: placeholder]: scalar_in[0] = surface id;
 *    returns success and does nothing (the backing store is
 *    zeroed in place is NOT done: purge is a no-op in v1, the
 *    memory stays mapped).
 * Any other selector returns kIOReturnUnsupported.
 * A scalar_in that is too short, or a struct_in that is too short
 * for the selector, returns kIOReturnBadArgument. An unknown
 * surface id returns kIOReturnNotFound.
 */
void iosurface_connect_method(mach_port_name_t conn_port,
                              iokit_method_call *call);

/*
 * IOConnectMapMemory (2863) backing for IOSurfaceRoot connections
 * (Candidate A). memory_type is the surface id [guess]; the
 * returned address is the surface's mapped base, usable directly
 * by the caller in-process. Returns KERN_SUCCESS, or
 * KERN_INVALID_NAME (unknown connection), KERN_INVALID_ARGUMENT
 * (bad memory_type) or KERN_FAILURE (surface not mapped).
 */
int iosurface_map_memory(mach_port_name_t conn_port, uint32_t memory_type,
                         uint64_t *addr_out, uint64_t *size_out);

/* The memfd (target) or host handle for a surface, for the later
 * presenter zero-copy handoff over @ioswine-surface. Returns
 * KERN_SUCCESS or KERN_INVALID_NAME. On the host the fd is the
 * opaque handle from iosurface_host_alloc. */
int iosurface_fd(uint32_t surface_id, int *fd_out);

/* Number of live surfaces. */
uint32_t iosurface_surface_count(void);

/* Copy one surface entry out (host tests and other lsk code).
 * Returns KERN_SUCCESS or KERN_INVALID_NAME. */
int iosurface_get_surface(uint32_t surface_id, iosurface_surface *out);

/*
 * Host platform hook (tests only). On the target
 * (-DIOSURFACE_HOST not defined) iosurface.c uses memfd_create +
 * ftruncate + mmap directly and these two functions are never
 * referenced. On the host the test provides them: alloc a
 * zero-filled buffer of size bytes, fill *fd_out with an opaque
 * handle (-1 if the test has none) and *ptr_out with the buffer;
 * free releases both. Declared here so the host test knows the
 * exact signatures to provide; no function pointers are involved.
 */
int iosurface_host_alloc(uint64_t size, int *fd_out, void **ptr_out);
void iosurface_host_free(int fd, void *ptr, uint64_t size);
