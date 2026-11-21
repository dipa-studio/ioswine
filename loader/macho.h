/* Mach-O definitions (subset) and the loader's image API. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MH_MAGIC_64        0xfeedfacfu
#define CPU_TYPE_ARM64     0x0100000c
#define CPU_SUBTYPE_MASK   0x00ffffff
#define CPU_SUBTYPE_ARM64E 2
#define MH_EXECUTE         2
#define MH_DYLIB           6

#define LC_REQ_DYLD             0x80000000u
#define LC_SEGMENT_64           0x19
#define LC_UNIXTHREAD           0x5
#define LC_LOAD_DYLIB           0xc
#define LC_LOAD_WEAK_DYLIB      (0x18 | LC_REQ_DYLD)
#define LC_REEXPORT_DYLIB       (0x1f | LC_REQ_DYLD)
#define LC_LAZY_LOAD_DYLIB      0x20
#define LC_LOAD_UPWARD_DYLIB    (0x23 | LC_REQ_DYLD)
#define LC_DYLD_INFO            0x22
#define LC_DYLD_INFO_ONLY       (0x22 | LC_REQ_DYLD)
#define LC_MAIN                 (0x28 | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS  (0x34 | LC_REQ_DYLD)

struct mach_header_64 {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
    uint32_t cmd, cmdsize;
    char     segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    int32_t  maxprot, initprot;
    uint32_t nsects, flags;
};
struct section_64 {
    char     sectname[16], segname[16];
    uint64_t addr, size;
    uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct entry_point_command { uint32_t cmd, cmdsize; uint64_t entryoff, stacksize; };
struct linkedit_data_command { uint32_t cmd, cmdsize, dataoff, datasize; };
struct dyld_info_command {
    uint32_t cmd, cmdsize;
    uint32_t rebase_off, rebase_size, bind_off, bind_size, weak_bind_off, weak_bind_size,
             lazy_bind_off, lazy_bind_size, export_off, export_size;
};

/* Chained fixups (mach-o/fixup-chains.h) */
struct dyld_chained_fixups_header {
    uint32_t fixups_version, starts_offset, imports_offset, symbols_offset,
             imports_count, imports_format, symbols_format;
};
struct dyld_chained_starts_in_image { uint32_t seg_count; uint32_t seg_info_offset[]; };
struct dyld_chained_starts_in_segment {
    uint32_t size;
    uint16_t page_size, pointer_format;
    uint64_t segment_offset;
    uint32_t max_valid_pointer;
    uint16_t page_count;
    uint16_t page_start[];
};
#define DYLD_CHAINED_PTR_START_NONE  0xFFFF
#define DYLD_CHAINED_PTR_START_MULTI 0x8000
#define DYLD_CHAINED_PTR_START_LAST  0x8000

enum {
    DYLD_CHAINED_PTR_ARM64E              = 1,
    DYLD_CHAINED_PTR_64                  = 2,
    DYLD_CHAINED_PTR_64_OFFSET           = 6,
    DYLD_CHAINED_PTR_ARM64E_USERLAND     = 9,
    DYLD_CHAINED_PTR_ARM64E_USERLAND24   = 12,
};

/* ARM thread state flavor for LC_UNIXTHREAD */
#define ARM_THREAD_STATE64 6

#define IMG_MAX_SEGS 16
struct img_seg {
    char     name[17];
    uint64_t addr, size;   /* runtime address (slid) and vmsize */
    int      prot;         /* Linux PROT_* derived from initprot */
    int      final;        /* file part already mapped with final protection */
};

enum { ENTRY_NONE, ENTRY_MAIN, ENTRY_UNIXTHREAD };

struct image {
    const char *path;
    int         fd;
    const uint8_t *file;   /* whole file mapped read-only, for linkedit parsing */
    uint64_t    file_size;
    int64_t     slide;     /* runtime addr - vmaddr */
    uint64_t    base;      /* runtime address of the mach header (__TEXT start) */
    int         arm64e;
    int         nsegs;
    struct img_seg segs[IMG_MAX_SEGS];
    int         entry_kind;
    uint64_t    entry;     /* runtime pc */
    uint64_t    stacksize; /* LC_MAIN stacksize, 0 = default */
    const struct linkedit_data_command *chained_cmd;
    const struct dyld_info_command      *dyldinfo_cmd;
    uint32_t    n_chained_imports;
    uint32_t    n_bind;
    uint32_t    n_lazy;
    uint32_t    n_weak;
};

/* Resolves imports: ordinal per Mach-O rules (>0 dylib index, 0 self, -1 main, -2 flat, -3 weak lookup). */
extern uint64_t (*macho_bind_resolver)(const struct image *img, int ordinal, const char *sym, int weak);

/* Map the image (all segments RW), apply fixups, set final protections. Dies on error. */
void macho_load(const char *path, int64_t slide, struct image *img);

/* Map image segments only (does not apply fixups). */
void macho_map(const char *path, int64_t slide, struct image *img);

/* Apply fixups (chained or classic) and set final protections. */
void macho_fixup(struct image *img);

/* Sign a pointer with the live process keys. key: 0=IA 1=IB 2=DA 3=DB. */
uint64_t pac_sign(uint64_t value, unsigned key, uint64_t discriminator);

/* Authenticate (strip) a signed pointer with the live keys; NOP when keys are disabled. */
uint64_t pac_auth(uint64_t value, unsigned key, uint64_t discriminator);

/* Make freshly written code visible to instruction fetch. */
void flush_icache(uint64_t start, uint64_t end);

/* Make page-aligned code [lo,hi) writable as private anonymous copies, then seal it RX again. */
void text_make_writable(uint64_t lo, uint64_t hi);
void text_seal(uint64_t lo, uint64_t hi);
