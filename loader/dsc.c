/* dyld shared cache loader (ARCHITECTURE.md §8, §12).
   M2a: eager slide-info processing. TODO(M2c): lazy per-page rebase via SIGSEGV to avoid ~750 MB COW. */
#include "dsc.h"
#include "macho.h"
#include "rt.h"
#include "sys.h"

extern uint64_t g_pagesize;
uint64_t dsc_stat_plain, dsc_stat_auth;
struct dsc_auth_sample dsc_samples[DSC_NSAMPLES];
int dsc_nsamples;

/* dyld_cache_header offsets (verified against 23G90) */
#define H_MAPPING_OFF        0x10
#define H_MAPPING_CNT        0x14
#define H_SR_START           0xe0
#define H_MAPSLIDE_OFF       0x138
#define H_MAPSLIDE_CNT       0x13c
#define H_SUBCACHE_OFF       0x188
#define H_SUBCACHE_CNT       0x18c
#define H_IMAGES_OFF         0x1c0
#define H_IMAGES_CNT         0x1c4

struct map_slide { uint64_t address, size, file_off, slide_off, slide_size, flags; uint32_t maxprot, initprot; };
struct subcache_entry { uint8_t uuid[16]; uint64_t vm_off; char suffix[32]; };
struct image_info { uint64_t address, mtime, inode; uint32_t path_off, pad; };
struct slide5 { uint32_t version, page_size, page_starts_count, pad; uint64_t value_add; uint16_t page_starts[]; };
#define V5_NO_REBASE 0xffff

#define LC_ID_DYLIB            0xd
#define LC_DYLD_EXPORTS_TRIE   (0x33 | LC_REQ_DYLD)
struct dylib_command { uint32_t cmd, cmdsize, name_off, timestamp, cur, compat; };

static const uint8_t *g_hdr;     /* main cache header, read-only file mapping */
/* slide-info mappings kept for dsc_auth_meta() (subcache files stay mapped read-only) */
#define MAX_SLIDE_MAPS 512
static struct { uint64_t address, size; const struct slide5 *si; const uint8_t *orig; } g_smaps[MAX_SLIDE_MAPS];
static int g_nsmaps;
static uint64_t g_hdr_size;

static int load_header(const char *path, const uint8_t **out, uint64_t *size, long *fdout) {
    long fd = sys4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) return (int)fd;
    uint8_t st[128];
    sys2(SYS_fstat, fd, st);
    *size = *(uint64_t *)(st + 48);
    void *m = sys_mmap(0, *size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (MAP_FAILED_P(m)) die("mmap %s", path);
    *out = m;
    *fdout = fd;
    return 0;
}

static void rebase_mapping(const uint8_t *file, const struct map_slide *m) {
    const struct slide5 *si = (const void *)(file + m->slide_off);
    if (si->version != 5) die("slide info version %u (want 5) at 0x%lx", si->version, m->address);
    for (uint32_t p = 0; p < si->page_starts_count; p++) {
        uint16_t start = si->page_starts[p];
        if (start == V5_NO_REBASE) continue;
        uint8_t *page = (uint8_t *)(m->address + (uint64_t)p * si->page_size);
        uint64_t off = start;
        for (;;) {
            uint64_t *loc = (uint64_t *)(page + off), raw = *loc, val;
            uint64_t next = (raw >> 52) & 0x7ff;
            uint64_t target = si->value_add + (raw & 0x3ffffffffull);   /* slide 0 */
            if (raw >> 63) {
                uint16_t div = (raw >> 34) & 0xffff;
                int addr_div = (raw >> 50) & 1, key_is_data = (raw >> 51) & 1;
                uint64_t d = addr_div ? (((uint64_t)loc & 0x0000ffffffffffffull) | ((uint64_t)div << 48)) : div;
                val = pac_sign(target, key_is_data ? 2 : 0, d);
                if (dsc_nsamples < DSC_NSAMPLES && (dsc_stat_auth % 100003) == 0)
                    dsc_samples[dsc_nsamples++] = (struct dsc_auth_sample){ (uint64_t)loc, target, d, key_is_data ? 2 : 0 };
                dsc_stat_auth++;
            } else {
                val = target | (((raw >> 34) & 0xff) << 56);
                dsc_stat_plain++;
            }
            *loc = val;
            if (!next) break;
            off += next * 8;
            if (off >= si->page_size) die("slide chain leaves page at 0x%lx", (uint64_t)page);
        }
    }
}

static void map_file(const char *path, int is_main) {
    const uint8_t *f;
    uint64_t size;
    long fd;
    int r = load_header(path, &f, &size, &fd);
    if (r < 0) die("open %s: %d", path, r);
    uint32_t mo = *(const uint32_t *)(f + H_MAPSLIDE_OFF), mc = *(const uint32_t *)(f + H_MAPSLIDE_CNT);
    if (!mc) die("%s: no mapping_with_slide info", path);
    const struct map_slide *ms = (const void *)(f + mo);
    for (uint32_t i = 0; i < mc; i++) {
        const struct map_slide *m = &ms[i];
        if ((m->address | m->file_off | m->size) & (g_pagesize - 1)) die("unaligned cache mapping 0x%lx", m->address);
        if (m->file_off + m->size > size) die("cache mapping past EOF in %s", path);
        int prot = (m->initprot & 1 ? PROT_READ : 0) | (m->initprot & 2 ? PROT_WRITE : 0) | (m->initprot & 4 ? PROT_EXEC : 0);
        int mprot = m->slide_size ? PROT_READ | PROT_WRITE : prot;
        void *a = sys_mmap((void *)m->address, m->size, mprot, MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, m->file_off);
        if (a != (void *)m->address) die("map cache 0x%lx+0x%lx from %s: %ld", m->address, m->size, path, (long)a);
        if (m->slide_size) {
            if (g_nsmaps == MAX_SLIDE_MAPS) die("too many slide mappings");
            g_smaps[g_nsmaps].address = m->address;
            g_smaps[g_nsmaps].size = m->size;
            g_smaps[g_nsmaps].si = (const void *)(f + m->slide_off);
            g_smaps[g_nsmaps].orig = f + m->file_off;   /* unrebased bytes in the read-only file view */
            g_nsmaps++;
            rebase_mapping(f, m);
            if (prot != mprot && sys3(SYS_mprotect, m->address, m->size, prot) < 0) die("mprotect cache");
        }
    }
    if (is_main) { g_hdr = f; g_hdr_size = size; }
    sys1(SYS_close, fd);
}

void dsc_load(const char *path) {
    map_file(path, 1);
    uint32_t so = *(const uint32_t *)(g_hdr + H_SUBCACHE_OFF), sc = *(const uint32_t *)(g_hdr + H_SUBCACHE_CNT);
    const struct subcache_entry *se = (const void *)(g_hdr + so);
    size_t pl = strlen(path);
    char sub[512];
    if (pl + 33 > sizeof sub) die("cache path too long");
    for (uint32_t i = 0; i < sc; i++) {
        memcpy(sub, path, pl);
        size_t k = 0;
        while (k < 31 && se[i].suffix[k]) { sub[pl + k] = se[i].suffix[k]; k++; }
        sub[pl + k] = 0;
        map_file(sub, 0);
    }
    LOG("dsc: %u subcaches, %lu plain + %lu auth pointers rebased\n", sc, dsc_stat_plain, dsc_stat_auth);
}

/* ---------- images and exports ---------- */

#define MAX_DSC_ALIASES 32
static struct dsc_alias_entry g_aliases[MAX_DSC_ALIASES];
static int g_naliases = 0;

int dsc_get_aliases(const struct dsc_alias_entry **out_entries, int *out_count) {
    if (out_entries) *out_entries = g_aliases;
    if (out_count) *out_count = g_naliases;
    return g_naliases;
}

static void record_alias(const char *from, const char *to) {
    for (int i = 0; i < g_naliases; i++) {
        if (!strcmp(g_aliases[i].from, from)) return;
    }
    if (g_naliases < MAX_DSC_ALIASES) {
        size_t lf = strlen(from), lt = strlen(to);
        if (lf < sizeof(g_aliases[0].from) && lt < sizeof(g_aliases[0].to)) {
            memcpy(g_aliases[g_naliases].from, from, lf + 1);
            memcpy(g_aliases[g_naliases].to, to, lt + 1);
            g_naliases++;
        }
    }
    ldprintf(2, "[dyld] alias: %s -> %s\n", from, to);
    LOG("alias: %s -> %s\n", from, to);
}

uint64_t dsc_image_raw(const char *name) {
    if (!g_hdr || !name) return 0;
    uint32_t io = *(const uint32_t *)(g_hdr + H_IMAGES_OFF), ic = *(const uint32_t *)(g_hdr + H_IMAGES_CNT);
    const struct image_info *ii = (const void *)(g_hdr + io);
    for (uint32_t i = 0; i < ic; i++)
        if (ii[i].path_off < g_hdr_size && !strcmp((const char *)g_hdr + ii[i].path_off, name)) return ii[i].address;
    return 0;
}

const char *dsc_resolve_alias(const char *name) {
    if (!name) return 0;
    /* 1. Explicit aliases */
    if (!strcmp(name, "/usr/lib/swift/libswiftNetwork.dylib")) {
        const char *tgt = "/System/Library/Frameworks/Network.framework/Network";
        if (dsc_image_raw(tgt)) {
            record_alias(name, tgt);
            return tgt;
        }
    }
    if (!strcmp(name, "/usr/lib/swift/libswiftWebKit.dylib")) {
        const char *tgt = "/System/Library/Frameworks/WebKit.framework/WebKit";
        if (dsc_image_raw(tgt)) {
            record_alias(name, tgt);
            return tgt;
        }
    }
    /* 2. Generic rule: /usr/lib/swift/libswiftFoo.dylib -> /System/Library/Frameworks/Foo.framework/Foo */
    const char *pfx = "/usr/lib/swift/libswift";
    size_t lp = 23; /* strlen(pfx) */
    const char *sfx = ".dylib";
    size_t ls = 6;  /* strlen(sfx) */
    size_t l = strlen(name);
    if (l > lp + ls && !strncmp(name, pfx, lp) && !strcmp(name + l - ls, sfx)) {
        size_t flen = l - lp - ls;
        static char cand[256];
        const char *fwpfx = "/System/Library/Frameworks/";
        size_t lfp = 27; /* strlen(fwpfx) */
        if (lfp + flen + 11 + flen + 1 < sizeof(cand)) {
            memcpy(cand, fwpfx, lfp);
            memcpy(cand + lfp, name + lp, flen);
            memcpy(cand + lfp + flen, ".framework/", 11);
            memcpy(cand + lfp + flen + 11, name + lp, flen);
            cand[lfp + flen + 11 + flen] = '\0';
            if (dsc_image_raw(cand)) {
                record_alias(name, cand);
                return cand;
            }
        }
    }
    return 0;
}

uint64_t dsc_image(const char *name) {
    if (!name) return 0;
    uint64_t a = dsc_image_raw(name);
    if (a) return a;
    const char *alt = dsc_resolve_alias(name);
    if (alt) return dsc_image_raw(alt);
    return 0;
}

static uint64_t uleb(const uint8_t **p) {
    uint64_t v = 0; unsigned s = 0; uint8_t b;
    do { b = *(*p)++; v |= (uint64_t)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    return v;
}

/* Returns the n-th (1-based) dependent dylib install name, counting all dylib load commands. */
const char *dsc_dep_name(uint64_t mh, uint64_t ordinal) {
    const struct mach_header_64 *h = (const void *)mh;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    uint64_t n = 0;
    for (uint32_t i = 0; i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        uint32_t c = ((const struct load_command *)cp)->cmd;
        if (c == LC_LOAD_DYLIB || c == LC_LOAD_WEAK_DYLIB || c == LC_REEXPORT_DYLIB ||
            c == LC_LOAD_UPWARD_DYLIB || c == LC_LAZY_LOAD_DYLIB)
            if (++n == ordinal) return (const char *)cp + ((const struct dylib_command *)cp)->name_off;
    }
    return 0;
}

static uint64_t lookup_depth(uint64_t mh, const char *sym, int depth);

static uint64_t trie_lookup(uint64_t mh, const uint8_t *trie, uint32_t trie_size, const char *sym, int depth) {
    const uint8_t *p = trie, *end = trie + trie_size;
    const char *s = sym;
    while (p < end) {
        const uint8_t *node = p;
        uint64_t tsize = uleb(&p);
        if (!*s) {
            if (!tsize) return 0;
            uint64_t flags = uleb(&p);
            if (flags & 0x08) {                     /* EXPORT_SYMBOL_FLAGS_REEXPORT */
                uint64_t ord = uleb(&p);
                const char *imp = (const char *)p;
                const char *dn = dsc_dep_name(mh, ord);
                uint64_t dmh = dn ? dsc_image(dn) : 0;
                if (!dmh) die("re-export of %s: dylib ordinal %lu not found", sym, ord);
                return lookup_depth(dmh, *imp ? imp : sym, depth + 1);
            }
            if (flags & 0x10) return mh + uleb(&p); /* stub-and-resolver: use the stub */
            uint64_t v = uleb(&p);
            return (flags & 3) == 2 ? v : mh + v;   /* absolute or image-relative */
        }
        p = node;
        uleb(&p);
        p += tsize;
        uint8_t nchild = *p++;
        const uint8_t *next = 0;
        for (uint8_t c = 0; c < nchild; c++) {
            const char *edge = (const char *)p;
            size_t el = strlen(edge);
            p += el + 1;
            uint64_t child = uleb(&p);
            if (!next && !strncmp(s, edge, el)) { next = trie + child; s += el; }
        }
        if (!next) return 0;
        p = next;
    }
    return 0;
}

static uint64_t lookup_depth(uint64_t mh, const char *sym, int depth) {
    if (depth > 8) die("re-export depth exceeded for %s", sym);
    const struct mach_header_64 *h = (const void *)mh;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    uint64_t le_slide = 0;          /* linkedit runtime vmaddr - fileoff */
    int64_t slide = 0;              /* nonzero for images outside the cache (our dylib) */
    const struct linkedit_data_command *et = 0;
    const struct dyld_info_command *di = 0;
    for (uint32_t i = 0; i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        const struct load_command *lc = (const void *)cp;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sc = (const void *)lc;
            if (!strcmp(sc->segname, "__TEXT")) slide = (int64_t)(mh - sc->vmaddr);
            if (!strcmp(sc->segname, "__LINKEDIT")) le_slide = sc->vmaddr - sc->fileoff;
        }
        else if (lc->cmd == LC_DYLD_EXPORTS_TRIE) et = (const void *)lc;
        else if (lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) di = (const void *)lc;
    }
    le_slide += slide;   /* __TEXT precedes __LINKEDIT in every image */
    uint64_t r = 0;
    if (et && et->datasize) r = trie_lookup(mh, (const uint8_t *)(le_slide + et->dataoff), et->datasize, sym, depth);
    else if (di && di->export_size) r = trie_lookup(mh, (const uint8_t *)(le_slide + di->export_off), di->export_size, sym, depth);
    if (r) return r;
    /* umbrella re-exports (LC_REEXPORT_DYLIB) are not in the trie */
    cp = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        if (((const struct load_command *)cp)->cmd != LC_REEXPORT_DYLIB) continue;
        uint64_t dmh = dsc_image((const char *)cp + ((const struct dylib_command *)cp)->name_off);
        if (dmh && (r = lookup_depth(dmh, sym, depth + 1))) return r;
    }
    return 0;
}

uint64_t dsc_lookup(uint64_t mh, const char *sym) { return lookup_depth(mh, sym, 0); }

/* Walk the original (unrebased) slide chain of the page containing addr; report the auth metadata of the
   pointer at addr. Returns 1 if it is an auth pointer, 0 otherwise. */
int dsc_auth_meta(uint64_t addr, uint16_t *div, int *addr_div, unsigned *key) {
    for (int i = 0; i < g_nsmaps; i++) {
        if (addr < g_smaps[i].address || addr >= g_smaps[i].address + g_smaps[i].size) continue;
        const struct slide5 *si = g_smaps[i].si;
        uint64_t pi = (addr - g_smaps[i].address) / si->page_size;
        uint16_t start = si->page_starts[pi];
        if (start == V5_NO_REBASE) return 0;
        const uint8_t *page = g_smaps[i].orig + pi * si->page_size;
        uint64_t want = (addr - g_smaps[i].address) % si->page_size, off = start;
        for (;;) {
            uint64_t raw = *(const uint64_t *)(page + off);
            if (off == want) {
                if (!(raw >> 63)) return 0;
                *div = (raw >> 34) & 0xffff;
                *addr_div = (raw >> 50) & 1;
                *key = (raw >> 51) & 1 ? 2 : 0;
                return 1;
            }
            uint64_t next = (raw >> 52) & 0x7ff;
            if (!next || off > want) return 0;
            off += next * 8;
        }
    }
    return 0;
}

const char *dsc_image_path(uint64_t mh) {
    uint32_t io = *(const uint32_t *)(g_hdr + H_IMAGES_OFF), ic = *(const uint32_t *)(g_hdr + H_IMAGES_CNT);
    const struct image_info *ii = (const void *)(g_hdr + io);
    for (uint32_t i = 0; i < ic; i++)
        if (ii[i].address == mh && ii[i].path_off < g_hdr_size) return (const char *)g_hdr + ii[i].path_off;
    return 0;
}
