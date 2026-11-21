/* Lookups in the shared cache's prebuilt ObjC hash tables.
   Implementation of loader/objc_opt.h. Freestanding, loader runtime only. */
#include "objc_opt.h"
#include "rt.h"

#define CACHE_BASE       0x180000000ull
#define OBJC_OPT_HDR_VA  0x1fa261610ull

/* Modern dyld shared cache Objective-C optimization header (version 2) */
struct objc_opt_header {
    uint32_t version;
    uint32_t flags;
    uint64_t header_info_ro_cache_offset;
    uint64_t header_info_rw_cache_offset;
    uint64_t selector_hash_table_cache_offset;
    uint64_t class_hash_table_cache_offset;
    uint64_t protocol_hash_table_cache_offset;
    uint64_t relative_method_selector_base;
};

/* Dyld perfect hash table header (32 bytes) */
struct objc_opt_table_header {
    uint32_t version;
    uint32_t capacity;
    uint32_t count;
    uint32_t shift;
    uint32_t mask;
    uint32_t pad;
    uint64_t salt;
};

static inline uint64_t read_u64(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint64_t)p[0] |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

/* Bob Jenkins lookup8 (64-bit Jenkins hash / mix64) matching dyld at 0x18011eb20 */
static inline void mix64(uint64_t *a, uint64_t *b, uint64_t *c) {
    *a -= *b; *a -= *c; *a ^= (*c >> 43);
    *b -= *c; *b -= *a; *b ^= (*a << 9);
    *c -= *a; *c -= *b; *c ^= (*b >> 8);
    *a -= *b; *a -= *c; *a ^= (*c >> 38);
    *b -= *c; *b -= *a; *b ^= (*a << 23);
    *c -= *a; *c -= *b; *c ^= (*b >> 5);
    *a -= *b; *a -= *c; *a ^= (*c >> 35);
    *b -= *c; *b -= *a; *b ^= (*a << 49);
    *c -= *a; *c -= *b; *c ^= (*b >> 11);
    *a -= *b; *a -= *c; *a ^= (*c >> 12);
    *b -= *c; *b -= *a; *b ^= (*a << 18);
    *c -= *a; *c -= *b; *c ^= (*b >> 22);
}

static uint64_t lookup8(const uint8_t *k, size_t length, uint64_t level) {
    uint64_t a = level;
    uint64_t b = level;
    uint64_t c = 0x9e3779b97f4a7c13ull;
    size_t l = length;
    const uint8_t *p = k;

    while (l >= 24) {
        a += read_u64(p);
        b += read_u64(p + 8);
        c += read_u64(p + 16);
        mix64(&a, &b, &c);
        p += 24;
        l -= 24;
    }

    c += length;
    switch (l) {
    case 23: c += ((uint64_t)p[22] << 56); __attribute__((fallthrough));
    case 22: c += ((uint64_t)p[21] << 48); __attribute__((fallthrough));
    case 21: c += ((uint64_t)p[20] << 40); __attribute__((fallthrough));
    case 20: c += ((uint64_t)p[19] << 32); __attribute__((fallthrough));
    case 19: c += ((uint64_t)p[18] << 24); __attribute__((fallthrough));
    case 18: c += ((uint64_t)p[17] << 16); __attribute__((fallthrough));
    case 17: c += ((uint64_t)p[16] << 8);  __attribute__((fallthrough));
    case 16: b += ((uint64_t)p[15] << 56); __attribute__((fallthrough));
    case 15: b += ((uint64_t)p[14] << 48); __attribute__((fallthrough));
    case 14: b += ((uint64_t)p[13] << 40); __attribute__((fallthrough));
    case 13: b += ((uint64_t)p[12] << 32); __attribute__((fallthrough));
    case 12: b += ((uint64_t)p[11] << 24); __attribute__((fallthrough));
    case 11: b += ((uint64_t)p[10] << 16); __attribute__((fallthrough));
    case 10: b += ((uint64_t)p[9] << 8);   __attribute__((fallthrough));
    case 9:  b += (uint64_t)p[8];          __attribute__((fallthrough));
    case 8:  a += ((uint64_t)p[7] << 56);  __attribute__((fallthrough));
    case 7:  a += ((uint64_t)p[6] << 48);  __attribute__((fallthrough));
    case 6:  a += ((uint64_t)p[5] << 40);  __attribute__((fallthrough));
    case 5:  a += ((uint64_t)p[4] << 32);  __attribute__((fallthrough));
    case 4:  a += ((uint64_t)p[3] << 24);  __attribute__((fallthrough));
    case 3:  a += ((uint64_t)p[2] << 16);  __attribute__((fallthrough));
    case 2:  a += ((uint64_t)p[1] << 8);   __attribute__((fallthrough));
    case 1:  a += (uint64_t)p[0];          __attribute__((fallthrough));
    case 0:  break;
    }

    mix64(&a, &b, &c);
    return c;
}

/* Performs lookup in a dyld ObjCHashTable (class, protocol, or selector table).
   Returns 1 and fills *out_idx if found, 0 if not found. */
static int find_table_index(const uint8_t *tbl, const char *name, uint32_t *out_idx) {
    if (!tbl || !name) return 0;
    const struct objc_opt_table_header *hdr = (const struct objc_opt_table_header *)tbl;
    if (hdr->capacity == 0) return 0;

    size_t len = strlen(name);
    uint64_t h = lookup8((const uint8_t *)name, len, hdr->salt);
    uint32_t h_shift = (hdr->shift == 64) ? 0 : (uint32_t)(h >> hdr->shift);
    uint32_t tab_idx = (uint32_t)(h & hdr->mask);

    const uint32_t *scramble = (const uint32_t *)(tbl + 32);
    const uint8_t *tab = tbl + 0x420;
    uint32_t scramble_val = scramble[tab[tab_idx]];
    uint32_t index = (h_shift ^ scramble_val);

    if (index >= hdr->capacity) return 0;

    const uint8_t *checkbytes = tab + (hdr->mask + 1);
    uint8_t expected_cb = (uint8_t)(((uint8_t)name[0] << 5) | (len & 0x1f));
    if (checkbytes[index] != expected_cb) return 0;

    const int32_t *offsets = (const int32_t *)(checkbytes + hdr->capacity);
    int32_t off = offsets[index];
    if (off == 0) return 0;

    const char *actual_name = (const char *)tbl + off;
    if (strcmp(name, actual_name) != 0) return 0;

    *out_idx = index;
    return 1;
}

/* Helper to visit all objects (classes or protocols) named `name` */
static int find_objects(const uint8_t *tbl, const char *name, int (*cb)(uint64_t, void *), void *ctx) {
    if (!tbl || !cb) return 0;
    uint32_t idx = 0;
    if (!find_table_index(tbl, name, &idx)) return 0;

    const struct objc_opt_table_header *hdr = (const struct objc_opt_table_header *)tbl;
    const uint8_t *checkbytes = tbl + 0x420 + (hdr->mask + 1);
    const int32_t *offsets = (const int32_t *)(checkbytes + hdr->capacity);
    const uint64_t *entries = (const uint64_t *)(offsets + hdr->capacity);

    uint64_t entry = entries[idx];
    int visited = 0;

    if ((entry & 1) == 0) {
        /* Single class entry: bits 1..47 = objectCacheOffset */
        uint64_t obj_off = (entry >> 1) & 0x7fffffffffffull;
        uint64_t cls = CACHE_BASE + obj_off;
        cb(cls, ctx);
        visited = 1;
    } else {
        /* Duplicate list: bits 48..63 = duplicateCount, bits 1..47 = duplicateIndex */
        uint32_t dup_count = (uint32_t)(entry >> 48);
        uint64_t dup_idx = (entry >> 1) & 0x7fffffffffffull;
        const uint8_t *dup_base = (const uint8_t *)(entries + hdr->capacity);
        const uint8_t *dup_entries_raw = dup_base + 4;

        for (uint32_t i = 0; i < dup_count; i++) {
            uint64_t e = read_u64(dup_entries_raw + (dup_idx + i) * 8);
            uint64_t obj_off = (e >> 1) & 0x7fffffffffffull;
            uint64_t cls = CACHE_BASE + obj_off;
            visited++;
            if (cb(cls, ctx) != 0) break;
        }
    }
    return visited;
}

int objc_opt_find_class(const char *name, int (*cb)(uint64_t cls, void *ctx), void *ctx) {
    const struct objc_opt_header *opt = (const struct objc_opt_header *)OBJC_OPT_HDR_VA;
    if (!opt->class_hash_table_cache_offset) return 0;
    const uint8_t *tbl = (const uint8_t *)(CACHE_BASE + opt->class_hash_table_cache_offset);
    return find_objects(tbl, name, cb, ctx);
}

int objc_opt_find_protocol(const char *name, int (*cb)(uint64_t proto, void *ctx), void *ctx) {
    const struct objc_opt_header *opt = (const struct objc_opt_header *)OBJC_OPT_HDR_VA;
    if (!opt->protocol_hash_table_cache_offset) return 0;
    const uint8_t *tbl = (const uint8_t *)(CACHE_BASE + opt->protocol_hash_table_cache_offset);
    return find_objects(tbl, name, cb, ctx);
}

uint64_t objc_opt_find_selector(const char *name) {
    const struct objc_opt_header *opt = (const struct objc_opt_header *)OBJC_OPT_HDR_VA;
    if (!opt->selector_hash_table_cache_offset) return 0;

    const uint8_t *tbl = (const uint8_t *)(CACHE_BASE + opt->selector_hash_table_cache_offset);
    uint32_t idx = 0;
    if (!find_table_index(tbl, name, &idx)) return 0;

    const struct objc_opt_table_header *hdr = (const struct objc_opt_table_header *)tbl;
    const uint8_t *checkbytes = tbl + 0x420 + (hdr->mask + 1);
    const int32_t *offsets = (const int32_t *)(checkbytes + hdr->capacity);
    int32_t off = offsets[idx];
    return (uint64_t)((const char *)tbl + off);
}

/* dyld4 objc_clsopt classCount(): occupied entries + duplicate-list length (u32 right after the entries array). */
uint32_t objc_opt_class_count(void) {
    const struct objc_opt_header *opt = (const struct objc_opt_header *)OBJC_OPT_HDR_VA;
    if (!opt->class_hash_table_cache_offset) return 0;
    const uint8_t *tbl = (const uint8_t *)(CACHE_BASE + opt->class_hash_table_cache_offset);
    const struct objc_opt_table_header *hdr = (const struct objc_opt_table_header *)tbl;
    const uint8_t *checkbytes = tbl + 0x420 + (hdr->mask + 1);
    const int32_t *offsets = (const int32_t *)(checkbytes + hdr->capacity);
    const uint64_t *entries = (const uint64_t *)(offsets + hdr->capacity);
    uint32_t dups = *(const uint32_t *)(entries + hdr->capacity);
    return hdr->count + dups;
}
