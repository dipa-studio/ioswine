/* Swap the cache's libsystem_kernel for ours (ARCHITECTURE.md §8, §12 M2b).
   Every code export entry of Apple's copy becomes a jump to our symbol of the same name:
     gap >= 16: ldr x16, #8 ; br x16 ; .quad target
     gap <  16: b <veneer>  (veneer of the form above, placed in an "island" inside the dead original
                __text, always within the ±128 MiB reach of b).
   Data exports stay in the cache (other images already point at them). */
#include "dsc.h"
#include "lsk.h"
#include "macho.h"
#include "rt.h"
#include "sys.h"

extern uint64_t g_pagesize;
#define MAX_EXPORTS 4096
struct exp { uint64_t addr, flags; uint32_t name; };
static struct exp *g_exp;
static char *g_names;
static uint32_t g_nexp, g_names_used;
#define NAMES_SIZE (256 * 1024)

static uint64_t uleb(const uint8_t **p) {
    uint64_t v = 0; unsigned s = 0; uint8_t b;
    do { b = *(*p)++; v |= (uint64_t)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    return v;
}

/* DFS over the export trie collecting (name, address, flags). */
static void walk(uint64_t mh, const uint8_t *trie, const uint8_t *node, char *buf, size_t len, int depth) {
    if (depth > 128) die("export trie too deep");
    const uint8_t *p = node;
    uint64_t tsize = uleb(&p);
    const uint8_t *children = p + tsize;
    if (tsize) {
        uint64_t flags = uleb(&p);
        uint64_t addr = 0;
        if (!(flags & 0x08)) {                       /* skip re-exports */
            if (flags & 0x10) addr = mh + uleb(&p);
            else { uint64_t v = uleb(&p); addr = (flags & 3) == 2 ? v : mh + v; }
            if (g_nexp == MAX_EXPORTS || g_names_used + len + 1 > NAMES_SIZE) die("too many exports");
            g_exp[g_nexp] = (struct exp){ addr, flags, g_names_used };
            memcpy(g_names + g_names_used, buf, len);
            g_names[g_names_used + len] = 0;
            g_names_used += len + 1;
            g_nexp++;
        }
    }
    p = children;
    uint8_t n = *p++;
    for (uint8_t i = 0; i < n; i++) {
        const char *edge = (const char *)p;
        size_t el = strlen(edge);
        p += el + 1;
        uint64_t off = uleb(&p);
        if (len + el >= 1024) die("export name too long");
        memcpy(buf + len, edge, el);
        walk(mh, trie, trie + off, buf, len + el, depth + 1);
    }
}

static const struct section_64 *find_text(uint64_t mh, const uint8_t **trie, uint32_t *trie_size) {
    const struct mach_header_64 *h = (const void *)mh;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    const struct section_64 *text = 0;
    uint64_t le = 0;
    uint32_t off = 0, size = 0;
    for (uint32_t i = 0; i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        const struct load_command *lc = (const void *)cp;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sc = (const void *)lc;
            if (!strcmp(sc->segname, "__LINKEDIT")) le = sc->vmaddr - sc->fileoff;
            const struct section_64 *s = (const void *)(sc + 1);
            for (uint32_t k = 0; k < sc->nsects; k++)
                if (!strcmp(s[k].segname, "__TEXT") && !strncmp(s[k].sectname, "__text", 16)) text = &s[k];
        } else if (lc->cmd == (0x33 | LC_REQ_DYLD)) {
            off = ((const struct linkedit_data_command *)lc)->dataoff;
            size = ((const struct linkedit_data_command *)lc)->datasize;
        } else if ((lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) && !size) {
            off = ((const struct dyld_info_command *)lc)->export_off;
            size = ((const struct dyld_info_command *)lc)->export_size;
        }
    }
    if (!text || !size) die("libsystem_kernel: no __text or export trie");
    *trie = (const uint8_t *)(le + off);
    *trie_size = size;
    return text;
}

static void sort_exp(void) {   /* insertion sort by address; n ~ 1600 */
    for (uint32_t i = 1; i < g_nexp; i++) {
        struct exp e = g_exp[i];
        uint32_t j = i;
        while (j && g_exp[j - 1].addr > e.addr) { g_exp[j] = g_exp[j - 1]; j--; }
        g_exp[j] = e;
    }
}

static void put_abs_jump(uint32_t *at, uint64_t target) {
    at[0] = 0x58000050;            /* ldr x16, #8 */
    at[1] = 0xd61f0200;            /* br x16 */
    *(uint64_t *)(at + 2) = target;
}

void lsk_swap(uint64_t our_mh, uint64_t *out_patched) {
    uint64_t lsk = dsc_image("/usr/lib/system/libsystem_kernel.dylib");
    if (!lsk) die("cache has no libsystem_kernel");
    g_exp = sys_mmap(0, MAX_EXPORTS * sizeof *g_exp + NAMES_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (MAP_FAILED_P(g_exp)) die("lsk mmap");
    g_names = (char *)(g_exp + MAX_EXPORTS);
    const uint8_t *trie;
    uint32_t trie_size;
    const struct section_64 *text = find_text(lsk, &trie, &trie_size);
    char buf[1024];
    walk(lsk, trie, trie, buf, 0, 0);
    sort_exp();
    uint64_t tlo = text->addr, thi = text->addr + text->size;

    /* island: tail of the export with the largest gap */
    uint64_t best = 0, best_gap = 0;
    for (uint32_t i = 0; i < g_nexp; i++) {
        uint64_t a = g_exp[i].addr;
        if (a < tlo || a >= thi) continue;
        uint64_t nx = thi;
        for (uint32_t j = i + 1; j < g_nexp; j++) if (g_exp[j].addr > a) { nx = g_exp[j].addr; break; }
        if (nx - a > best_gap) { best_gap = nx - a; best = a; }
    }
    uint64_t island = best + 16, island_end = best + best_gap;

    uint64_t plo = tlo & ~(g_pagesize - 1), phi = (thi + g_pagesize - 1) & ~(g_pagesize - 1);
    text_make_writable(plo, phi);
    uint64_t patched = 0, missing = 0, data = 0, veneers = 0;
    for (uint32_t i = 0; i < g_nexp; i++) {
        uint64_t a = g_exp[i].addr;
        const char *name = g_names + g_exp[i].name;
        if (a < tlo || a >= thi) { data++; continue; }
        if (i && g_exp[i - 1].addr == a) continue;                 /* alias already patched */
        uint64_t target = dsc_lookup(our_mh, name);
        if (!target) { missing++; LOG("lsk: no replacement for %s\n", name); continue; }
        uint64_t nx = thi;
        for (uint32_t j = i + 1; j < g_nexp; j++) if (g_exp[j].addr > a) { nx = g_exp[j].addr; break; }
        if (nx - a >= 16 || a == best) {
            put_abs_jump((uint32_t *)a, target);
        } else {
            if (island + 16 > island_end) die("lsk: trampoline island full");
            put_abs_jump((uint32_t *)island, target);
            int64_t d = (int64_t)(island - a);
            *(uint32_t *)a = 0x14000000 | ((uint32_t)(d >> 2) & 0x03ffffff);   /* b island */
            island += 16;
            veneers++;
        }
        patched++;
    }
    text_seal(plo, phi);
    LOG("lsk: %u exports, %lu code patched (%lu via island), %lu data kept, %lu without replacement\n",
        g_nexp, patched, veneers, data, missing);
    if (missing) die("lsk: %lu code exports have no replacement in our dylib", missing);

    /* Data exports stay in the cache. Our dylib reaches them through pointer variables named
       iosw_ref_<name without leading underscore> (Mach-O: _iosw_ref_...), filled here. */
    uint64_t refs = 0;
    for (uint32_t i = 0; i < g_nexp; i++) {
        uint64_t a = g_exp[i].addr;
        if (a >= tlo && a < thi) continue;
        const char *name = g_names + g_exp[i].name;
        char rn[300];
        size_t l = strlen(name);
        if (l > 280) continue;
        memcpy(rn, "_iosw_ref_", 10);
        memcpy(rn + 10, name + 1, l);           /* drop the leading '_' of the C symbol */
        uint64_t ref = dsc_lookup(our_mh, rn);
        if (ref) { *(uint64_t *)ref = a; refs++; }
        LOG("lsk: data export %s @0x%lx%s\n", name, a, ref ? " (ref bound)" : "");
    }
    LOG("lsk: %lu data refs bound\n", refs);
    if (out_patched) *out_patched = patched;
}
