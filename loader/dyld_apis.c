/* Fake dyld4::gAPIs object for libdyld (ARCHITECTURE.md §12).
   libdyld calls: obj = *gAPIs; vt = autda(*obj, blend(obj, 0x63fa)); braa(vt[slot], blend(&vt[slot], div_slot)).
   We build obj + vtable in our memory, sign with the live keys (NOPs in arm64 PAC mode), point gAPIs at it.
   Slot layout and diversities: gen/data/dyld_apis_23G90.tsv. */
#include "dyld.h"
#include "macho.h"
#include "rt.h"
#include "sys.h"

#define GAPIS_ADDR      0x1ec954040ull   /* __ZN5dyld45gAPIsE in libdyld, __TPRO_CONST */
#define GAPIS_OBJ_DISC  0x63fa
#define NSLOTS          136

extern uint64_t g_pagesize;
uint64_t dyld_exe_mh(void);
#include "dyld.h"
#include "dsc.h"
#include "objc_opt.h"
const char *dyld_exe_path(void);

static const struct { uint16_t div; uint8_t addr_div; const char *name; } g_slots[NSLOTS] = {
#define IA 0
#define DA 2
#include "../gen/data/dyld_apis_23G90.inc"
#undef IA
#undef DA
};

static uint64_t g_obj[2] __attribute__((aligned(16)));
static uint64_t g_vtab[NSLOTS] __attribute__((aligned(16)));

/* 256 trampolines of 8 bytes: movz x16, #i ; b api_common */
extern char api_tramps[];
__asm__(".text\n.p2align 3\n.globl api_tramps\napi_tramps:\n"
        ".set iosw_i, 0\n.rept 256\n\tmovz x16, #iosw_i\n\tb api_common\n.set iosw_i, iosw_i + 1\n.endr\n"
        "api_common:\n"
        "\tstp x29, x30, [sp, #-16]!\n\tmov x29, sp\n\tsub sp, sp, #64\n"
        "\tstp x0, x1, [sp]\n\tstp x2, x3, [sp, #16]\n\tstp x4, x5, [sp, #32]\n\tstp x6, x7, [sp, #48]\n"
        "\tmov x0, x16\n\tmov x1, sp\n\tbl dyld_api_call\n"
        "\tldr x1, [sp, #8]\n"            /* second return register (16-byte struct returns) */
        "\tadd sp, sp, #64\n\tldp x29, x30, [sp], #16\n\tret\n");

static int is(const char *full, const char *m) {
    const char *p = full;
    if (!strncmp(p, "dyld4::APIs::", 13)) p += 13;
    size_t l = strlen(m);
    return !strncmp(p, m, l) && p[l] == '(';
}

#define SDK_26 0x001a0000ull
#define CACHE_BASE 0x180000000ull
#define CACHE_SIZE 0x142544000ull
#define OBJC_HDR_RO 0x1fa261658ull   /* cache base + ObjCOptimizationHeader.headerInfoROCacheOffset */
#define OBJC_HDR_RW 0x1ec978000ull   /* cache base + headerInfoRWCacheOffset */

/* ---- image helpers ---- */
static const struct section_64 *find_sect(uint64_t mh, const char *sect, int64_t *slide) {
    const struct mach_header_64 *h = (const void *)mh;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    *slide = 0;
    for (uint32_t i = 0; i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        const struct segment_command_64 *sc = (const void *)cp;
        if (sc->cmd != LC_SEGMENT_64) continue;
        if (!strcmp(sc->segname, "__TEXT")) *slide = (int64_t)(mh - sc->vmaddr);
        const struct section_64 *se = (const void *)(sc + 1);
        for (uint32_t k = 0; k < sc->nsects; k++)
            if (!strncmp(se[k].sectname, sect, 16)) return &se[k];
    }
    return 0;
}

static int image_index_containing(uint64_t addr) {
    for (uint32_t i = 0; i < dyld_image_count(); i++) {
        const struct mach_header_64 *h = (const void *)dyld_image_mh(i);
        const uint8_t *cp = (const uint8_t *)(h + 1);
        int64_t slide = 0;
        for (uint32_t k = 0; k < h->ncmds; k++, cp += ((const struct load_command *)cp)->cmdsize) {
            const struct segment_command_64 *sc = (const void *)cp;
            if (sc->cmd != LC_SEGMENT_64) continue;
            if (!strcmp(sc->segname, "__TEXT")) slide = (int64_t)((uint64_t)h - sc->vmaddr);
            if (!sc->filesize && !strcmp(sc->segname, "__PAGEZERO")) continue;
            if (addr >= sc->vmaddr + slide && addr < sc->vmaddr + slide + sc->vmsize) return (int)i;
        }
    }
    return -1;
}

/* _dyld_section_location_kind -> section name. Values VERIFIED against the cache's own PrebuiltLoader
   SectionLocations arrays (23G90): 6 imageinfo, 7 selrefs, 9 classrefs, 10 superrefs, 11 protorefs, 12 classlist,
   13 nlclslist, 14 stublist, 15 catlist, 16 catlist2, 17 nlcatlist, 18 protolist. 0-5, 8, 19, 20 inferred. */
static const char *section_kind_name(uint64_t kind) {
    switch (kind) {
    case 0: return "__swift5_protos";  case 1: return "__swift5_proto";   case 2: return "__swift5_types";
    case 3: return "__swift5_replace"; case 4: return "__swift5_replac2"; case 5: return "__swift5_acfuncs";
    case 6: return "__objc_imageinfo"; case 7: return "__objc_selrefs";   case 8: return "__objc_msgrefs";
    case 9: return "__objc_classrefs"; case 10: return "__objc_superrefs"; case 11: return "__objc_protorefs";
    case 12: return "__objc_classlist"; case 13: return "__objc_nlclslist"; case 14: return "__objc_stublist";
    case 15: return "__objc_catlist";  case 16: return "__objc_catlist2"; case 17: return "__objc_nlcatlist";
    case 18: return "__objc_protolist"; case 19: return "__objc_fork_ok"; case 20: return "__objc_rawisa";
    default: return 0;
    }
}

/* ---- objc ---- */
struct objc_callbacks { uint64_t version, mapped, init, unmapped, patch_class; };
struct objc_mapped_info { uint64_t mh; const char *path; uint64_t section_meta; uint32_t flags, pad; };
static struct objc_callbacks g_objc;
static struct objc_mapped_info *g_infos;
static uint32_t g_ninfos;

static int has_sect(uint64_t mh, const char *n) { int64_t sl; const struct section_64 *s = find_sect(mh, n, &sl); return s && s->size; }

/* objc's map_images 3rd parameter is a BLOCK on iOS 26 (dyld4 `_dyld_objc_mark_image_mutable`:
 * void (^)(uint32_t index into the info array of that call)). libobjc invokes it via [blk+16] (blraa). It must make the
 * image's __DATA_CONST writable (libobjc then patches class/category data in place). */
static uint32_t g_mut_first;
static void mark_image_mutable_invoke(void *blk, uint32_t idx) {
    (void)blk;
    uint64_t mh = g_infos[g_mut_first + idx].mh;
    const struct mach_header_64 *h = (const void *)mh;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    int64_t slide = 0;
    for (uint32_t i = 0; i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        const struct segment_command_64 *sc = (const void *)cp;
        if (sc->cmd != LC_SEGMENT_64) continue;
        if (!strcmp(sc->segname, "__TEXT")) slide = (int64_t)(mh - sc->vmaddr);
        if (strcmp(sc->segname, "__DATA_CONST") && strcmp(sc->segname, "__AUTH_CONST")) continue;
        uint64_t a = (sc->vmaddr + slide) & ~(uint64_t)0x3fff, e = (sc->vmaddr + slide + sc->vmsize + 0x3fff) & ~(uint64_t)0x3fff;
        long r = sys3(SYS_mprotect, (long)a, (long)(e - a), PROT_READ | PROT_WRITE);
        char seg[17];
        memcpy(seg, sc->segname, 16);
        seg[16] = 0;
        LOG("objc: mark_image_mutable(%u) %s %s -> %ld\n", idx, g_infos[g_mut_first + idx].path, seg, r);
    }
}
static const struct { uint64_t size, copy_helper_unused; } g_mut_desc = { 32, 0 };
static struct { void *isa; int32_t flags, reserved; void (*invoke)(void *, uint32_t); const void *desc; } g_mut_block = {
    0, (1 << 28) /* BLOCK_IS_GLOBAL */, 0, mark_image_mutable_invoke, &g_mut_desc };

#define MAX_INFOS 2048
static void objc_map_from(uint32_t from, int run_init) {
    if (!g_objc.mapped) return;
    if (!g_infos) g_infos = sys_mmap(0, MAX_INFOS * sizeof *g_infos, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t first = g_ninfos, n = dyld_image_count();
    for (uint32_t i = from; i < n && g_ninfos < MAX_INFOS; i++) {
        uint64_t mh = dyld_image_mh(i);
        if (!has_sect(mh, "__objc_imageinfo")) continue;
        int is_cache = (mh >= CACHE_BASE && mh < CACHE_BASE + CACHE_SIZE);
        uint32_t flags = is_cache ? 1 : 0;   /* bit 0 = isPreoptimized (cache only). Bit 1 (TPRO) must be 0 */
        if (has_sect(mh, "__objc_nlclslist") || has_sect(mh, "__objc_nlcatlist")) flags |= 4;
        g_infos[g_ninfos++] = (struct objc_mapped_info){ mh, dyld_image_path(i), 0, flags, 0 };   /* no Loader*: NULL */
    }
    if (g_ninfos == first) return;
    LOG("objc: mapped(%u images)\n", g_ninfos - first);
    g_mut_first = first;
    if (!g_mut_block.isa) g_mut_block.isa = (void *)dyld_dlsym((uint64_t)-2, "NSConcreteGlobalBlock");
    ((void (*)(uint32_t, const struct objc_mapped_info *, void *))g_objc.mapped)(g_ninfos - first, g_infos + first, &g_mut_block);
    if (run_init && g_objc.init)
        for (uint32_t i = first; i < g_ninfos; i++)
            if (g_infos[i].flags & 4) ((void (*)(const struct objc_mapped_info *))g_objc.init)(&g_infos[i]);
}

static void objc_register(const struct objc_callbacks *cb) {
    if (cb->version != 4) die("objc callbacks version %lu (want 4)", cb->version);
    g_objc = *cb;
    objc_map_from(0, 0);
}

void dyld_objc_notify_new(uint32_t from) { objc_map_from(from, 1); }

void dyld_objc_run_inits(void) {
    if (!g_objc.init) return;
    for (uint32_t i = 0; i < g_ninfos; i++)
        if (g_infos[i].flags & 4) {
            LOG("objc: init %s\n", g_infos[i].path);
            ((void (*)(const struct objc_mapped_info *))g_objc.init)(&g_infos[i]);
        }
}

/* Block callbacks: invoke(block, void *obj, bool isLoaded, bool *stop); invoke at +16, signed IA with
   address diversity (disc 0) when keys are enabled. */
static int block_cb(uint64_t obj, void *ctx) {
    uint64_t *blk = ctx;
    uint64_t fn = pac_auth(blk[2], 0, (uint64_t)&blk[2]);
    uint8_t stop = 0;
    int loaded = image_index_containing(obj) >= 0;
    ((void (*)(void *, uint64_t, int, uint8_t *))fn)(blk, obj, loaded, &stop);
    return stop;
}

uint64_t dyld_api_call(uint64_t slot, uint64_t *a);
uint64_t dyld_api_call(uint64_t slot, uint64_t *a) {
    const char *n = slot < NSLOTS ? g_slots[slot].name : "?";
    uint64_t *x = a + 1;   /* x0 = this (the APIs object); real arguments start at x1 */
    uint64_t r = 0, r1 = a[1];
    int idx;
    if (is(n, "_libdyld_initialize")) r = 0;
    else if (is(n, "_dyld_image_count")) r = dyld_image_count();
    else if (is(n, "_dyld_get_image_header")) r = dyld_image_mh((uint32_t)x[0]);
    else if (is(n, "_dyld_get_prog_image_header")) r = dyld_exe_mh();
    else if (is(n, "_dyld_get_image_vmaddr_slide")) { int64_t sl = 0; if (x[0] < dyld_image_count()) find_sect(dyld_image_mh((uint32_t)x[0]), "__text", &sl); r = (uint64_t)sl; }
    else if (is(n, "_dyld_get_image_slide")) { int64_t sl = 0; if (x[0]) find_sect(x[0], "__text", &sl); r = (uint64_t)sl; }
    else if (is(n, "_dyld_find_unwind_sections")) {   /* (addr, dyld_unwind_sections*) -> bool */
        idx = image_index_containing(x[0]);
        if (idx >= 0 && x[1]) {
            uint64_t mh = dyld_image_mh(idx), *o = (uint64_t *)x[1];
            int64_t sl;
            const struct section_64 *eh = find_sect(mh, "__eh_frame", &sl), *cu = find_sect(mh, "__unwind_info", &sl);
            o[0] = mh;
            o[1] = eh ? eh->addr + sl : 0; o[2] = eh ? eh->size : 0;
            o[3] = cu ? cu->addr + sl : 0; o[4] = cu ? cu->size : 0;
            r = 1;
        }
    }
    else if (is(n, "_dyld_get_image_name")) r = (uint64_t)dyld_image_path((uint32_t)x[0]);
    else if (is(n, "dyld_image_path_containing_address")) { idx = image_index_containing(x[0]); r = idx < 0 ? 0 : (uint64_t)dyld_image_path(idx); }
    else if (is(n, "dyld_get_program_sdk_version") || is(n, "dyld_get_program_min_os_version") ||
             is(n, "dyld_get_sdk_version") || is(n, "dyld_get_min_os_version") ||
             is(n, "dyld_get_program_sdk_version_token")) r = SDK_26;
    else if (is(n, "dyld_program_sdk_at_least") || is(n, "dyld_program_minos_at_least") ||
             is(n, "dyld_sdk_at_least") || is(n, "dyld_version_token_at_least")) r = 1;
    else if (is(n, "dyld_get_active_platform") || is(n, "dyld_get_base_platform")) r = 2;   /* PLATFORM_IOS */
    else if (is(n, "dyld_process_is_restricted") || is(n, "dyld_shared_cache_some_image_overridden")) r = 0;
    else if (is(n, "_dyld_stack_range")) {
        uint64_t sp_approx = (uint64_t)&slot;
        uint64_t top = (sp_approx + 0x1000000ull) & ~0xfffull;
        uint64_t bot = top - 0x800000ull;
        if (x[0]) *(uint64_t *)x[0] = bot;
        if (x[1]) *(uint64_t *)x[1] = top;
        r = 0;
    }
    else if (is(n, "_dyld_register_func_for_add_image")) {
        for (uint32_t i = 0; i < dyld_image_count(); i++) ((void (*)(uint64_t, long))x[0])(dyld_image_mh(i), 0);
    }
    else if (is(n, "_dyld_register_func_for_remove_image")) r = 0;
    else if (is(n, "_dyld_register_for_bulk_image_loads")) {   /* cb(count, mhs[], paths[]) */
        uint32_t c = dyld_image_count();
        static uint64_t mhs[2048], paths[2048];
        for (uint32_t i = 0; i < c && i < 2048; i++) { mhs[i] = dyld_image_mh(i); paths[i] = (uint64_t)dyld_image_path(i); }
        ((void (*)(uint32_t, uint64_t *, uint64_t *))x[0])(c, mhs, paths);
    }
    else if (is(n, "_NSGetExecutablePath")) {   /* (buf, uint32_t *bufsize) -> 0 / -1 */
        const char *p = dyld_exe_path();
        uint32_t need = (uint32_t)strlen(p) + 1, *sz = (uint32_t *)x[1];
        if (*sz < need) { *sz = need; r = (uint64_t)-1; } else { memcpy((void *)x[0], p, need); r = 0; }
    }
    else if (is(n, "_dyld_shared_cache_real_path")) { uint64_t mh = dsc_image((const char *)x[0]); r = mh ? (uint64_t)dsc_image_path(mh) : 0; }
    else if (is(n, "_dyld_swift_optimizations_version")) r = 0;
    else if (is(n, "_dyld_objc_class_count")) r = objc_opt_class_count();
    else if (is(n, "dyld_image_header_containing_address")) { idx = image_index_containing(x[0]); r = idx < 0 ? 0 : dyld_image_mh(idx); }
    else if (is(n, "dladdr")) {
        idx = image_index_containing(x[0]);
        uint64_t *info = (uint64_t *)x[1];   /* dli_fname, dli_fbase, dli_sname, dli_saddr */
        if (idx >= 0 && info) { info[0] = (uint64_t)dyld_image_path(idx); info[1] = dyld_image_mh(idx); info[2] = 0; info[3] = 0; r = 1; }
    }
    else if (is(n, "_dyld_get_shared_cache_uuid")) { if (x[0]) memcpy((void *)x[0], (const void *)(CACHE_BASE + 0x58), 16); r = 1; }
    else if (is(n, "_dyld_get_shared_cache_range")) { if (x[0]) *(uint64_t *)x[0] = CACHE_SIZE; r = CACHE_BASE; }
    else if (is(n, "_dyld_shared_cache_contains_path")) r = 0;   /* TODO: real lookup; 0 is the safe answer for dlopen paths */
    else if (is(n, "_dyld_is_memory_immutable")) r = 0;
    else if (is(n, "_dyld_objc_uses_large_shared_cache")) r = 1;
    else if (is(n, "_dyld_objc_register_callbacks")) objc_register((const struct objc_callbacks *)x[0]);
    else if (is(n, "_dyld_for_objc_header_opt_ro")) r = OBJC_HDR_RO;
    else if (is(n, "_dyld_for_objc_header_opt_rw")) r = OBJC_HDR_RW;
    else if (is(n, "_dyld_is_preoptimized_objc_image_loaded")) r = 1;
    else if (is(n, "_dyld_lookup_section_info")) {   /* (mh, section_meta, kind) -> {ptr, size} */
        if (x[2] > 20) { r = 0; r1 = (uint64_t)-1; goto done; }
        const char *sn = section_kind_name(x[2]);
        int64_t sl;
        const struct section_64 *se = sn ? find_sect(x[0], sn, &sl) : 0;
        r = se ? se->addr + sl : 0;
        r1 = se ? se->size : 0;
    }
    else if (is(n, "_dyld_for_each_objc_class")) r = 0, objc_opt_find_class((const char *)x[0], block_cb, (void *)x[1]);
    else if (is(n, "_dyld_for_each_objc_protocol")) r = 0, objc_opt_find_protocol((const char *)x[0], block_cb, (void *)x[1]);
    else if (is(n, "dlopen") || is(n, "dlopen_from") || is(n, "dlopen_audited")) r = dyld_dlopen((const char *)x[0]);
    else if (is(n, "dlsym")) r = dyld_dlsym(x[0], (const char *)x[1]);
    else if (is(n, "dlclose")) r = 0;
    else if (is(n, "dlerror")) r = 0;
    else if (is(n, "dlopen_preflight") || is(n, "_dyld_dlopen_preflight")) r = dsc_image((const char *)x[0]) != 0;
    else if (is(n, "_dyld_get_image_uuid")) {
        const struct mach_header_64 *h = (const void *)x[0];
        const uint8_t *cp = (const uint8_t *)(h + 1);
        for (uint32_t i = 0; h && i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize)
            if (((const struct load_command *)cp)->cmd == 0x1b) { memcpy((void *)x[1], cp + 8, 16); r = 1; break; }
    }
    else if (is(n, "dyld_has_inserted_or_interposing_libraries")) r = 0;
    else if (is(n, "_dyld_get_objc_selector")) r = objc_opt_find_selector((const char *)x[0]);
    else if (is(n, "_dyld_is_objc_constant") || is(n, "_dyld_get_swift_prespecialized_data") ||
             is(n, "_dyld_find_protocol_conformance") || is(n, "_dyld_find_foreign_type_protocol_conformance") ||
             is(n, "_dyld_has_preoptimized_swift_protocol_conformances") ||
             is(n, "_dyld_find_protocol_conformance_on_disk") ||
             is(n, "_dyld_find_foreign_type_protocol_conformance_on_disk")) r = 0;
    else ldprintf(2, "[dyld] api slot %lu %s -> 0 (stub)\n", slot, n);
done:
    LOG("dyld api %lu %s(0x%lx, 0x%lx, 0x%lx) = 0x%lx\n", slot, n, x[0], x[1], x[2], r);
    a[1] = r1;
    return r;
}

static uint64_t blend(uint64_t addr, uint16_t div) { return (addr & 0x0000ffffffffffffull) | ((uint64_t)div << 48); }

void dyld_apis_setup(void) {
    for (int i = 0; i < NSLOTS; i++) {
        uint64_t t = (uint64_t)(api_tramps + 8 * i);
        uint64_t d = g_slots[i].addr_div ? blend((uint64_t)&g_vtab[i], g_slots[i].div) : g_slots[i].div;
        g_vtab[i] = pac_sign(t, 0, d);
    }
    g_obj[0] = pac_sign((uint64_t)g_vtab, 2, blend((uint64_t)g_obj, GAPIS_OBJ_DISC));
    uint64_t pg = GAPIS_ADDR & ~(g_pagesize - 1);
    if (sys3(SYS_mprotect, pg, g_pagesize, PROT_READ | PROT_WRITE) < 0) die("gAPIs mprotect RW");
    *(uint64_t *)GAPIS_ADDR = (uint64_t)g_obj;
    sys3(SYS_mprotect, pg, g_pagesize, PROT_READ);
    LOG("dyld apis: gAPIs -> %p, %d slots\n", (void *)g_obj, NSLOTS);
}
