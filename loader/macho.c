/* Mach-O image mapping and fixups (ARCHITECTURE.md §3, §4, §6). */
#include "macho.h"
#include "rt.h"
#include "sys.h"

extern uint64_t g_pagesize;

static uint64_t rup(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int prot_from_vm(int vmprot) {
    int p = 0;
    if (vmprot & 1) p |= PROT_READ;
    if (vmprot & 2) p |= PROT_WRITE;
    if (vmprot & 4) p |= PROT_EXEC;
    return p;
}

/* ---------- cache maintenance ---------- */

void flush_icache(uint64_t start, uint64_t end) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dline = 4ull << ((ctr >> 16) & 0xf), iline = 4ull << (ctr & 0xf);
    for (uint64_t a = start & ~(dline - 1); a < end; a += dline)
        __asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t a = start & ~(iline - 1); a < end; a += iline)
        __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

/* ---------- PAC ---------- */
/* Encoded with fixed registers (x16 = value, x17 = modifier) so the loader can stay plain armv8.0:
   PACIA x16,x17 = 0xdac10230, PACIB = 0xdac10630, PACDA = 0xdac10a30, PACDB = 0xdac10e30. */
uint64_t pac_sign(uint64_t value, unsigned key, uint64_t disc) {
    register uint64_t x16 __asm__("x16") = value;
    register uint64_t x17 __asm__("x17") = disc;
    switch (key) {
    case 0: __asm__ volatile(".inst 0xdac10230" : "+r"(x16) : "r"(x17)); break;
    case 1: __asm__ volatile(".inst 0xdac10630" : "+r"(x16) : "r"(x17)); break;
    case 2: __asm__ volatile(".inst 0xdac10a30" : "+r"(x16) : "r"(x17)); break;
    default: __asm__ volatile(".inst 0xdac10e30" : "+r"(x16) : "r"(x17)); break;
    }
    return x16;
}

/* AUTIA/AUTIB/AUTDA/AUTDB x16, x17 */
uint64_t pac_auth(uint64_t value, unsigned key, uint64_t disc) {
    register uint64_t x16 __asm__("x16") = value;
    register uint64_t x17 __asm__("x17") = disc;
    switch (key) {
    case 0: __asm__ volatile(".inst 0xdac11230" : "+r"(x16) : "r"(x17)); break;
    case 1: __asm__ volatile(".inst 0xdac11630" : "+r"(x16) : "r"(x17)); break;
    case 2: __asm__ volatile(".inst 0xdac11a30" : "+r"(x16) : "r"(x17)); break;
    default: __asm__ volatile(".inst 0xdac11e30" : "+r"(x16) : "r"(x17)); break;
    }
    return x16;
}

static uint64_t blend(uint64_t loc, uint16_t div) {
    return (loc & 0x0000ffffffffffffull) | ((uint64_t)div << 48);
}

/* ---------- chained fixups ---------- */

static uint64_t g_fixup_count;
uint64_t (*macho_bind_resolver)(const struct image *img, int ordinal, const char *sym, int weak);

/* resolved chained imports of the image being loaded */
static uint64_t *g_imports;
static uint32_t g_nimports;

static uint64_t resolve_bind(const struct image *img, int ord, const char *sym, int weak) {
    if (!macho_bind_resolver) die("%s binds %s but no resolver is installed", img->path, sym);
    uint64_t a = macho_bind_resolver(img, ord, sym, weak);
    if (!a && !weak) die("unresolved symbol %s (ordinal %d) in %s", sym, ord, img->path);
    return a;
}

static uint64_t import_value(struct image *img, uint64_t ord) {
    if (ord >= g_nimports) die("bind ordinal %lu out of range in %s", ord, img->path);
    (void)img;
    return g_imports[ord];
}

static void fixup_chain(struct image *img, uint16_t fmt, uint8_t *loc) {
    for (;;) {
        uint64_t raw = *(uint64_t *)loc, next, val;
        switch (fmt) {
        case DYLD_CHAINED_PTR_64:
        case DYLD_CHAINED_PTR_64_OFFSET: {
            /* rebase: target:36 high8:8 reserved:7 next:12 bind:1 ; stride 4 */
            if (raw >> 63) {   /* bind: ordinal:24 addend:8 reserved:19 next:12 bind:1 */
                uint64_t t = import_value(img, raw & 0xffffff);
                *(uint64_t *)loc = t ? t + ((raw >> 24) & 0xff) : 0;
                g_fixup_count++;
                next = ((raw >> 51) & 0xfff) * 4;
                if (!next) return;
                loc += next;
                continue;
            }
            uint64_t target = raw & 0xfffffffffull, high8 = (raw >> 36) & 0xff;
            next = (raw >> 51) & 0xfff;
            val = (fmt == DYLD_CHAINED_PTR_64) ? target + img->slide : img->base + target;
            val |= high8 << 56;
            *(uint64_t *)loc = val;
            next *= 4;
            break;
        }
        case DYLD_CHAINED_PTR_ARM64E:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND24: {
            /* stride 8; bit63 auth, bit62 bind, bits 51..61 next */
            int auth = raw >> 63, bind = (raw >> 62) & 1;
            next = ((raw >> 51) & 0x7ff) * 8;
            if (bind) {
                uint64_t ord = fmt == DYLD_CHAINED_PTR_ARM64E_USERLAND24 ? raw & 0xffffff : raw & 0xffff;
                uint64_t t = import_value(img, ord);
                if (auth) {   /* ordinal, zero:16, diversity:16, addrDiv:1, key:2 */
                    uint16_t div = (raw >> 32) & 0xffff;
                    int addr_div = (raw >> 48) & 1;
                    unsigned key = (raw >> 49) & 3;
                    val = t ? pac_sign(t, key, addr_div ? blend((uint64_t)loc, div) : div) : 0;
                } else {      /* ordinal, zero, addend:19 (sign-extended) */
                    int64_t addend = (int64_t)(raw << 13) >> 45;
                    val = t ? t + addend : 0;
                }
                *(uint64_t *)loc = val;
                g_fixup_count++;
                if (!next) return;
                loc += next;
                continue;
            }
            if (auth) {
                /* target:32 (offset from base) diversity:16 addrDiv:1 key:2 */
                uint64_t target = raw & 0xffffffffull;
                uint16_t div = (raw >> 32) & 0xffff;
                int addr_div = (raw >> 48) & 1;
                unsigned key = (raw >> 49) & 3;
                uint64_t d = addr_div ? blend((uint64_t)loc, div) : div;
                val = pac_sign(img->base + target, key, d);
            } else {
                /* target:43 high8:8; ARM64E: vmaddr, USERLAND*: offset from base */
                uint64_t target = raw & 0x7ffffffffffull, high8 = (raw >> 43) & 0xff;
                val = (fmt == DYLD_CHAINED_PTR_ARM64E) ? target + img->slide : img->base + target;
                val |= high8 << 56;
            }
            *(uint64_t *)loc = val;
            break;
        }
        default:
            die("unsupported chained pointer_format %u", fmt);
        }
        g_fixup_count++;
        if (!next) return;
        loc += next;
    }
}

static void apply_chained_fixups(struct image *img, const struct linkedit_data_command *lc) {
    const uint8_t *blob = img->file + lc->dataoff;
    const struct dyld_chained_fixups_header *h = (const void *)blob;
    if (h->fixups_version != 0) die("chained fixups version %u", h->fixups_version);
    g_nimports = h->imports_count;
    if (g_nimports) {
        g_imports = sys_mmap(0, g_nimports * 8ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (MAP_FAILED_P(g_imports)) die("imports mmap");
        if (h->symbols_format != 0) die("compressed chained symbols unsupported");
        const uint8_t *imp = blob + h->imports_offset;
        const char *syms = (const char *)blob + h->symbols_offset;
        for (uint32_t i = 0; i < g_nimports; i++) {
            int ord, weak;
            uint32_t name_off;
            int64_t addend = 0;
            switch (h->imports_format) {
            case 1: { uint32_t v = ((const uint32_t *)imp)[i];
                      ord = (int8_t)(v & 0xff); weak = (v >> 8) & 1; name_off = v >> 9; break; }
            case 2: { const uint32_t *v = (const uint32_t *)imp + 2 * i;
                      ord = (int8_t)(v[0] & 0xff); weak = (v[0] >> 8) & 1; name_off = v[0] >> 9; addend = (int32_t)v[1]; break; }
            case 3: { const uint64_t *v = (const uint64_t *)imp + 2 * i;
                      ord = (int16_t)(v[0] & 0xffff); weak = (v[0] >> 16) & 1; name_off = (uint32_t)(v[0] >> 32); addend = (int64_t)v[1]; break; }
            default: die("chained imports_format %u", h->imports_format);
            }
            uint64_t a = resolve_bind(img, ord, syms + name_off, weak);
            g_imports[i] = a ? a + addend : 0;
        }
        LOG("%u imports resolved\n", g_nimports);
    }
    const struct dyld_chained_starts_in_image *si = (const void *)(blob + h->starts_offset);
    for (uint32_t s = 0; s < si->seg_count; s++) {
        if (!si->seg_info_offset[s]) continue;
        const struct dyld_chained_starts_in_segment *ss =
            (const void *)((const uint8_t *)si + si->seg_info_offset[s]);
        LOG("chained seg %u fmt=%u pages=%u pagesz=0x%x\n", s, ss->pointer_format, ss->page_count, ss->page_size);
        for (uint16_t p = 0; p < ss->page_count; p++) {
            uint16_t start = ss->page_start[p];
            if (start == DYLD_CHAINED_PTR_START_NONE) continue;
            if (start & DYLD_CHAINED_PTR_START_MULTI) die("chain start MULTI (32-bit formats) unsupported");
            uint8_t *loc = (uint8_t *)(img->base + ss->segment_offset + (uint64_t)p * ss->page_size + start);
            fixup_chain(img, ss->pointer_format, loc);
        }
    }
}

/* ---------- classic rebase opcodes (LC_DYLD_INFO) ---------- */

static uint64_t uleb(const uint8_t **p, const uint8_t *end) {
    uint64_t v = 0; unsigned shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        if (shift < 64) v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

static void apply_rebase_opcodes(struct image *img, const struct dyld_info_command *di) {
    const uint8_t *p = img->file + di->rebase_off, *end = p + di->rebase_size;
    uint64_t addr = 0;
    int seg = 0;
    while (p < end) {
        uint8_t op = *p & 0xf0, imm = *p & 0x0f;
        p++;
        uint64_t n, skip;
        switch (op) {
        case 0x00: return;                                  /* DONE */
        case 0x10: if (imm != 1) die("rebase type %u", imm); break; /* SET_TYPE_IMM (pointer) */
        case 0x20: seg = imm; if (seg >= img->nsegs) die("rebase seg %d", seg);
                   addr = img->segs[seg].addr + uleb(&p, end); break;
        case 0x30: addr += uleb(&p, end); break;
        case 0x40: addr += (uint64_t)imm * 8; break;
        case 0x50: case 0x60: case 0x70: case 0x80:
            n = (op == 0x50) ? imm : (op == 0x70 ? 1 : uleb(&p, end));
            skip = (op == 0x70 || op == 0x80) ? uleb(&p, end) : 0;
            while (n--) {
                *(uint64_t *)addr += img->slide;
                g_fixup_count++;
                addr += 8 + skip;
            }
            break;
        default: die("rebase opcode 0x%x", op);
        }
    }
}

/* count distinct symbols (opcode 0x40) in a dyldinfo bind stream */
static uint32_t count_bind_0x40(const uint8_t *p, const uint8_t *end, int is_lazy) {
    uint32_t c = 0;
    while (p < end) {
        uint8_t op = *p & 0xf0, imm = *p & 0x0f;
        p++;
        if (op == 0x00) { if (!is_lazy) break; }
        else if (op == 0x40) {
            c++;
            p += strlen((const char *)p) + 1;
        }
        else if (op == 0x20 || op == 0x70 || op == 0x80 || op == 0xa0) { uleb(&p, end); }
        else if (op == 0x60) {
            uint8_t b; do { b = *p++; } while (p < end && (b & 0x80));
        }
        else if (op == 0xc0) { uleb(&p, end); uleb(&p, end); }
        else if (op == 0xd0 && imm == 0) { uleb(&p, end); }
    }
    return c;
}

/* classic bind opcodes; kind: 0 = normal bind, 1 = lazy bind, 2 = weak bind (coalescing) */
static void apply_bind_opcodes(struct image *img, const uint8_t *p, const uint8_t *end, int kind) {
    uint64_t addr = 0;
    int ord = (kind == 2) ? -3 : 0;
    int weak = (kind == 2) ? 1 : 0;
    int64_t addend = 0;
    const char *sym = 0;
    while (p < end) {
        uint8_t op = *p & 0xf0, imm = *p & 0x0f;
        p++;
        uint64_t n, skip;
        switch (op) {
        case 0x00: if (kind == 1) break; return;                                  /* DONE (lazy: per-entry) */
        case 0x10: ord = imm; break;                                             /* DYLIB_ORDINAL_IMM */
        case 0x20: ord = (int)uleb(&p, end); break;                              /* DYLIB_ORDINAL_ULEB */
        case 0x30: ord = imm ? (int)(int8_t)(0xf0 | imm) : 0; break;             /* DYLIB_SPECIAL_IMM */
        case 0x40:
            sym = (const char *)p; p += strlen(sym) + 1;
            weak = (kind == 2) ? 1 : (imm & 1);
            break; /* SYMBOL_TRAILING_FLAGS_IMM */
        case 0x50: if (imm != 1) die("bind type %u", imm); break;                /* TYPE_IMM pointer */
        case 0x60: { uint64_t v = 0; unsigned sh = 0; uint8_t b;                 /* ADDEND_SLEB */
                     do { b = *p++; v |= (uint64_t)(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
                     if (sh < 64 && (b & 0x40)) v |= -(1ull << sh);
                     addend = (int64_t)v; break; }
        case 0x70: if (imm >= img->nsegs) die("bind seg %u", imm);               /* SET_SEGMENT_AND_OFFSET_ULEB */
                   addr = img->segs[imm].addr + uleb(&p, end); break;
        case 0x80: addr += uleb(&p, end); break;                                 /* ADD_ADDR_ULEB */
        case 0x90: case 0xa0: case 0xb0: case 0xc0: {                            /* DO_BIND variants */
            n = 1; skip = 0;
            if (op == 0xc0) { n = uleb(&p, end); skip = uleb(&p, end); }
            uint64_t v = resolve_bind(img, ord, sym, weak);
            while (n--) {
                *(uint64_t *)addr = v ? v + addend : 0;
                g_fixup_count++;
                addr += 8 + skip;
            }
            if (op == 0xa0) addr += uleb(&p, end);
            if (op == 0xb0) addr += (uint64_t)imm * 8;
            break;
        }
        default: die("bind opcode 0x%x", op);
        }
    }
}

/* ---------- loading ---------- */

void macho_map(const char *path, int64_t slide, struct image *img) {
    memset(img, 0, sizeof *img);
    img->path = path;
    img->slide = slide;
    long fd = sys4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) die("open %s: errno %ld", path, -fd);
    uint8_t st[128];
    if (sys2(SYS_fstat, fd, st) < 0) die("fstat %s", path);
    img->fd = (int)fd;
    img->file_size = *(uint64_t *)(st + 48);
    void *f = sys_mmap(0, img->file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (MAP_FAILED_P(f)) die("mmap file %s", path);
    img->file = f;

    const struct mach_header_64 *mh = f;
    if (img->file_size < sizeof *mh || mh->magic != MH_MAGIC_64) die("%s: not a 64-bit Mach-O (fat files unsupported)", path);
    if (mh->cputype != CPU_TYPE_ARM64) die("%s: cputype 0x%x is not arm64", path, mh->cputype);
    if (mh->filetype != MH_EXECUTE && mh->filetype != MH_DYLIB) die("%s: filetype %u unsupported", path, mh->filetype);
    img->arm64e = (mh->cpusubtype & CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E;

    uint64_t text_vmaddr = (uint64_t)-1;
    const uint8_t *cp = (const uint8_t *)(mh + 1), *cend = cp + mh->sizeofcmds;
    if ((uint64_t)(cend - (const uint8_t *)f) > img->file_size) die("load commands past EOF");

    /* pass 1: segments */
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const void *)cp;
        if (lc->cmdsize < 8 || cp + lc->cmdsize > cend) die("bad load command %u", i);
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            const struct segment_command_64 *sc = (const void *)lc;
            if (img->nsegs == IMG_MAX_SEGS) die("too many segments");
            struct img_seg *s = &img->segs[img->nsegs++];
            memcpy(s->name, sc->segname, 16);
            s->addr = sc->vmaddr + slide;
            s->size = sc->vmsize;
            s->prot = prot_from_vm(sc->initprot);
            if (sc->fileoff == 0 && sc->filesize) text_vmaddr = sc->vmaddr;
            if (!sc->vmsize || (sc->initprot == 0 && sc->maxprot == 0 && sc->filesize == 0)) {
                LOG("skip %s\n", s->name);   /* __PAGEZERO: left unmapped */
                s->size = 0;
                break;
            }
            if ((s->addr | sc->fileoff) & (g_pagesize - 1)) die("segment %s not page aligned", s->name);
            if (sc->fileoff + sc->filesize > img->file_size) die("segment %s past EOF", s->name);
            uint64_t fmap = rup(sc->filesize, g_pagesize), total = rup(sc->vmsize, g_pagesize);
            if (fmap > total) fmap = total;
            if (fmap) {
                /* Non-writable segments (text, linkedit) are mapped with their final protection straight
                   away: SELinux (execmod) forbids making a written file mapping executable for uid shell.
                   Fixups never target them. */
                s->final = !(s->prot & PROT_WRITE);
                void *m = sys_mmap((void *)s->addr, fmap, s->final ? s->prot : PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, sc->fileoff);
                if (m != (void *)s->addr) die("map %s at 0x%lx: %ld", s->name, s->addr, (long)m);
                if (!s->final && fmap > sc->filesize) memset((void *)(s->addr + sc->filesize), 0, fmap - sc->filesize);
            }
            if (total > fmap) {
                void *m = sys_mmap((void *)(s->addr + fmap), total - fmap, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (m != (void *)(s->addr + fmap)) die("zerofill %s: %ld", s->name, (long)m);
            }
            LOG("map %s 0x%lx-0x%lx file 0x%lx+0x%lx prot %d\n", s->name, s->addr, s->addr + total,
                sc->fileoff, sc->filesize, s->prot);
            break;
        }
        case LC_MAIN: {
            const struct entry_point_command *ep = (const void *)lc;
            img->entry_kind = ENTRY_MAIN;
            img->entry = ep->entryoff;          /* relocated after pass 1 */
            img->stacksize = ep->stacksize;
            break;
        }
        case LC_UNIXTHREAD: {
            const uint32_t *t = (const uint32_t *)(lc + 1);
            if (t[0] != ARM_THREAD_STATE64) die("LC_UNIXTHREAD flavor %u", t[0]);
            /* state after flavor,count: x0..x28, fp, lr, sp, pc */
            img->entry_kind = ENTRY_UNIXTHREAD;
            img->entry = *(const uint64_t *)((const uint8_t *)(t + 2) + 32 * 8) + slide;
            break;
        }
        case LC_DYLD_CHAINED_FIXUPS: img->chained_cmd = (const void *)lc; break;
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY: img->dyldinfo_cmd = (const void *)lc; break;
        default: break;
        }
        cp += lc->cmdsize;
    }
    if (text_vmaddr == (uint64_t)-1) die("no __TEXT segment");
    img->base = text_vmaddr + slide;
    if (img->entry_kind == ENTRY_MAIN) img->entry += img->base;
    if (img->entry_kind == ENTRY_NONE && mh->filetype == MH_EXECUTE) die("no entry point");

    /* Count binds for plan / diagnostics */
    if (img->chained_cmd) {
        const struct dyld_chained_fixups_header *ch = (const void *)(img->file + img->chained_cmd->dataoff);
        img->n_chained_imports = ch->imports_count;
    }
    if (img->dyldinfo_cmd) {
        const uint8_t *le = img->file;
        if (img->dyldinfo_cmd->bind_size)
            img->n_bind = count_bind_0x40(le + img->dyldinfo_cmd->bind_off, le + img->dyldinfo_cmd->bind_off + img->dyldinfo_cmd->bind_size, 0);
        if (img->dyldinfo_cmd->lazy_bind_size)
            img->n_lazy = count_bind_0x40(le + img->dyldinfo_cmd->lazy_bind_off, le + img->dyldinfo_cmd->lazy_bind_off + img->dyldinfo_cmd->lazy_bind_size, 1);
        if (img->dyldinfo_cmd->weak_bind_size)
            img->n_weak = count_bind_0x40(le + img->dyldinfo_cmd->weak_bind_off, le + img->dyldinfo_cmd->weak_bind_off + img->dyldinfo_cmd->weak_bind_size, 0);
    }
}

void macho_fixup(struct image *img) {
    g_fixup_count = 0;
    if (img->chained_cmd) apply_chained_fixups(img, img->chained_cmd);
    else if (img->dyldinfo_cmd) {
        apply_rebase_opcodes(img, img->dyldinfo_cmd);
        const uint8_t *le = img->file;
        if (img->dyldinfo_cmd->bind_size)
            apply_bind_opcodes(img, le + img->dyldinfo_cmd->bind_off, le + img->dyldinfo_cmd->bind_off + img->dyldinfo_cmd->bind_size, 0);
        if (img->dyldinfo_cmd->weak_bind_size)
            apply_bind_opcodes(img, le + img->dyldinfo_cmd->weak_bind_off, le + img->dyldinfo_cmd->weak_bind_off + img->dyldinfo_cmd->weak_bind_size, 2);
        if (img->dyldinfo_cmd->lazy_bind_size)
            apply_bind_opcodes(img, le + img->dyldinfo_cmd->lazy_bind_off, le + img->dyldinfo_cmd->lazy_bind_off + img->dyldinfo_cmd->lazy_bind_size, 1);
    }
    else if (img->slide) die("no fixup info but slide != 0");
    LOG("%lu fixups applied, arm64e=%d in %s\n", g_fixup_count, img->arm64e, img->path);

    for (int i = 0; i < img->nsegs; i++) {
        struct img_seg *s = &img->segs[i];
        if (!s->size || s->final) continue;
        uint64_t len = rup(s->size, g_pagesize);
        if (sys3(SYS_mprotect, s->addr, len, s->prot) < 0) die("mprotect %s", s->name);
        if (s->prot & PROT_EXEC) flush_icache(s->addr, s->addr + len);
    }
}

void macho_load(const char *path, int64_t slide, struct image *img) {
    macho_map(path, slide, img);
    macho_fixup(img);
    LOG("entry %s 0x%lx\n", img->entry_kind == ENTRY_MAIN ? "LC_MAIN" : "LC_UNIXTHREAD", img->entry);
}

/* ---------- patching file-backed code (SELinux execmod-safe) ---------- */
/* Replace [lo,hi) (page aligned) with anonymous RW copies; seal with text_seal() afterwards. */
void text_make_writable(uint64_t lo, uint64_t hi) {
    uint64_t len = hi - lo;
    void *tmp = sys_mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (MAP_FAILED_P(tmp)) die("text copy mmap");
    memcpy(tmp, (void *)lo, len);
    void *m = sys_mmap((void *)lo, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)lo) die("text anon remap 0x%lx: %ld", lo, (long)m);
    memcpy((void *)lo, tmp, len);
    sys2(SYS_munmap, tmp, len);
}

void text_seal(uint64_t lo, uint64_t hi) {
    if (sys3(SYS_mprotect, lo, hi - lo, PROT_READ | PROT_EXEC) < 0) die("text seal mprotect 0x%lx", lo);
    flush_icache(lo, hi);
}
