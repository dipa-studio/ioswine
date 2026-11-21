/* dyld-light: libSystem mode boot (ARCHITECTURE.md §12 M2c).
   Loader stack:  pac mode -> cache -> TLS patch -> commpage -> our libsystem_kernel + swap -> exe (binds)
                  -> dyld APIs object -> temporary TSD -> seccomp -> switch to the Darwin stack.
   Darwin stack:  libSystem.B initializers(argc, argv, envp, apple, &ProgramVars) -> exe initializers
                  -> main -> libSystem exit(). */
#include "commpage.h"
#include "dsc.h"
#include "dyld.h"
#include "lsk.h"
#include "macho.h"
#include "rt.h"
#include "sigsys.h"
#include "sys.h"

extern uint64_t g_pagesize;

#define LSK_BASE 0x300000000ull
#define PR_PAC_SET_ENABLED_KEYS 60

static struct image g_exe, g_lsk;
static uint64_t g_libsystem;

/* loaded image list: [0] = main executable, then the dependency closure in BFS order */
#define MAX_IMAGES 2048
static uint64_t g_img_mh[MAX_IMAGES];
static const char *g_img_path[MAX_IMAGES];
static uint8_t g_img_inited[MAX_IMAGES];
static uint8_t g_img_via_upward[MAX_IMAGES];   /* reached through an upward link: its upward links are not followed */
static uint32_t g_nimg;
static int g_boot_argc;
static char **g_boot_argv, **g_boot_envp, **g_boot_apple;

#define MAX_FRAMEWORKS 16
static struct image g_frameworks[MAX_FRAMEWORKS];
static char g_framework_install_names[MAX_FRAMEWORKS][128];
static char g_framework_paths[MAX_FRAMEWORKS][256];
static int g_nframeworks = 0;
static uint64_t g_framework_next_base = 0x310000000ull;

struct rpath_res_entry {
    char from[128];
    char to[256];
};
#define MAX_RPATH_RES 16
static struct rpath_res_entry g_rpath_res[MAX_RPATH_RES];
static int g_nrpath_res = 0;

static char g_app_dir[256];
static char g_exe_name[128];

static void set_app_dir_from_exe(const char *exe_path) {
    size_t l = strlen(exe_path);
    if (l >= sizeof(g_app_dir)) die("exe path too long: %s", exe_path);
    memcpy(g_app_dir, exe_path, l + 1);
    char *slash = 0;
    for (char *p = g_app_dir; *p; p++) if (*p == '/') slash = p;
    if (slash) {
        *slash = '\0';
        size_t nl = strlen(slash + 1);
        if (nl < sizeof(g_exe_name)) memcpy(g_exe_name, slash + 1, nl + 1);
        else g_exe_name[0] = '\0';
    } else {
        g_app_dir[0] = '.'; g_app_dir[1] = '\0';
        size_t nl = strlen(exe_path);
        if (nl < sizeof(g_exe_name)) memcpy(g_exe_name, exe_path, nl + 1);
    }
}

static int get_rpaths(uint64_t mh, const char *rpaths[], int max_rpaths) {
    const struct mach_header_64 *h = (const void *)mh;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    int count = 0;
    for (uint32_t i = 0; i < h->ncmds && count < max_rpaths; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        if (((const struct load_command *)cp)->cmd == 0x8000001c /* LC_RPATH */) {
            uint32_t off = ((const uint32_t *)cp)[2];
            rpaths[count++] = (const char *)cp + off;
        }
    }
    return count;
}

static uint64_t resolve_embedded_framework(const char *name, uint64_t parent_mh) {
    /* Check if already loaded */
    for (int k = 0; k < g_nframeworks; k++) {
        if (!strcmp(g_framework_install_names[k], name) || !strcmp(g_framework_paths[k], name))
            return g_frameworks[k].base;
    }

    const char *tail = 0;
    const char *rps[32];
    int nrps = 0;
    char loader_dir[256];
    loader_dir[0] = '.'; loader_dir[1] = '\0';
    if (parent_mh && parent_mh != g_exe.base) {
        for (int k = 0; k < g_nframeworks; k++) {
            if (g_frameworks[k].base == parent_mh) {
                size_t l = strlen(g_framework_paths[k]);
                if (l < sizeof(loader_dir)) {
                    memcpy(loader_dir, g_framework_paths[k], l + 1);
                    char *sl = 0;
                    for (char *p = loader_dir; *p; p++) if (*p == '/') sl = p;
                    if (sl) *sl = '\0';
                }
                break;
            }
        }
    }

    if (!strncmp(name, "@rpath/", 7)) {
        tail = name + 7;
        if (parent_mh) nrps = get_rpaths(parent_mh, rps, 16);
        if (parent_mh != g_exe.base) {
            int extra = get_rpaths(g_exe.base, rps + nrps, 32 - nrps);
            nrps += extra;
        }
    } else if (!strncmp(name, "@executable_path/", 17)) {
        tail = name + 17;
        rps[0] = "@executable_path";
        nrps = 1;
    } else if (!strncmp(name, "@loader_path/", 13)) {
        tail = name + 13;
        rps[0] = "@loader_path";
        nrps = 1;
    } else {
        return 0;
    }

    char cand[512];
    int found = 0;
    for (int r = 0; r < nrps; r++) {
        const char *rp = rps[r];
        char exp_rp[256];
        if (!strncmp(rp, "@executable_path", 16)) {
            size_t la = strlen(g_app_dir), lrest = strlen(rp + 16);
            if (la + lrest < sizeof(exp_rp)) {
                memcpy(exp_rp, g_app_dir, la);
                memcpy(exp_rp + la, rp + 16, lrest + 1);
            } else continue;
        } else if (!strncmp(rp, "@loader_path", 12)) {
            size_t ld = strlen(loader_dir), lrest = strlen(rp + 12);
            if (ld + lrest < sizeof(exp_rp)) {
                memcpy(exp_rp, loader_dir, ld);
                memcpy(exp_rp + ld, rp + 12, lrest + 1);
            } else continue;
        } else {
            size_t lrp = strlen(rp);
            if (lrp < sizeof(exp_rp)) memcpy(exp_rp, rp, lrp + 1);
            else continue;
        }

        size_t ler = strlen(exp_rp), lt = strlen(tail);
        if (ler + 1 + lt < sizeof(cand)) {
            memcpy(cand, exp_rp, ler);
            cand[ler] = '/';
            memcpy(cand + ler + 1, tail, lt + 1);
            long fd = sys4(SYS_openat, AT_FDCWD, cand, O_RDONLY, 0);
            if (fd >= 0) {
                sys1(SYS_close, fd);
                found = 1;
                break;
            }
        }
    }

    if (!found) return 0;

    /* Record resolution */
    int seen_res = 0;
    for (int k = 0; k < g_nrpath_res; k++) {
        if (!strcmp(g_rpath_res[k].from, name)) { seen_res = 1; break; }
    }
    if (!seen_res && g_nrpath_res < MAX_RPATH_RES) {
        size_t lf = strlen(name);
        if (lf < sizeof(g_rpath_res[0].from)) memcpy(g_rpath_res[g_nrpath_res].from, name, lf + 1);
        const char *rel = cand;
        size_t adlen = strlen(g_app_dir);
        if (!strncmp(cand, g_app_dir, adlen) && cand[adlen] == '/') rel = cand + adlen + 1;
        size_t lr = strlen(rel);
        if (lr < sizeof(g_rpath_res[0].to)) memcpy(g_rpath_res[g_nrpath_res].to, rel, lr + 1);
        g_nrpath_res++;
    }

    /* Check again if already mapped by path */
    for (int k = 0; k < g_nframeworks; k++) {
        if (!strcmp(g_framework_paths[k], cand)) return g_frameworks[k].base;
    }

    if (g_nframeworks >= MAX_FRAMEWORKS) die("too many embedded frameworks");
    struct image *fw = &g_frameworks[g_nframeworks];
    uint64_t base = g_framework_next_base;
    g_framework_next_base += 0x10000000ull; /* 256 MB spacing */

    macho_map(cand, (int64_t)base, fw);
    size_t lc = strlen(cand);
    if (lc < sizeof(g_framework_paths[0])) memcpy(g_framework_paths[g_nframeworks], cand, lc + 1);
    size_t ln = strlen(name);
    if (ln < sizeof(g_framework_install_names[0])) memcpy(g_framework_install_names[g_nframeworks], name, ln + 1);
    g_nframeworks++;
    return fw->base;
}

static void add_image(uint64_t mh, const char *path) {
    for (uint32_t i = 0; i < g_nimg; i++) if (g_img_mh[i] == mh) return;
    if (g_nimg == MAX_IMAGES) die("too many images");
    g_img_mh[g_nimg] = mh;
    g_img_path[g_nimg] = path;
    g_nimg++;
}

/* i-th dylib load command of mh: returns install name, sets *is_weak and *upward. */
static const char *dep_cmd_info(uint64_t mh, uint64_t ord, int *is_weak, int *upward) {
    const struct mach_header_64 *h = (const void *)mh;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    uint64_t n = 0;
    if (is_weak) *is_weak = 0;
    if (upward) *upward = 0;
    for (uint32_t i = 0; i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        uint32_t c = ((const struct load_command *)cp)->cmd;
        if (c == LC_LOAD_DYLIB || c == LC_LOAD_WEAK_DYLIB || c == LC_REEXPORT_DYLIB ||
            c == LC_LOAD_UPWARD_DYLIB || c == LC_LAZY_LOAD_DYLIB)
            if (++n == ord) {
                if (is_weak) *is_weak = (c == LC_LOAD_WEAK_DYLIB);
                if (upward) *upward = (c == LC_LOAD_UPWARD_DYLIB) ? 1 : (c == LC_LAZY_LOAD_DYLIB) ? 2 : 0;
                return (const char *)cp + ((const uint32_t *)cp)[2];
            }
    }
    return 0;
}

static uint64_t resolve_dylib_mh(const char *dn, uint64_t parent_mh, int is_weak) {
    if (!dn) return 0;
    if (dn[0] == '@') {
        uint64_t mh = resolve_embedded_framework(dn, parent_mh);
        if (mh) return mh;
    }
    uint64_t mh = dsc_image(dn);
    if (mh) return mh;
    if (is_weak) return 0;
    return 0;
}

/* Launch/dlopen set: downward links only (LOAD, WEAK, REEXPORT); upward and lazy links are not followed */
static void close_over_deps(uint32_t from) {
    for (uint32_t i = from; i < g_nimg; i++) {
        const char *dn;
        int up = 0, is_weak = 0;
        for (uint64_t ord = 1; (dn = dep_cmd_info(g_img_mh[i], ord, &is_weak, &up)); ord++) {
            if (up == 2 || (up && g_img_via_upward[i])) continue;
            uint64_t mh = resolve_dylib_mh(dn, g_img_mh[i], is_weak);
            if (!mh) continue;
            uint32_t before = g_nimg;
            const char *path = dsc_image_path(mh);
            if (!path) path = dn;
            add_image(mh, path);
            if (g_nimg > before) g_img_via_upward[g_nimg - 1] = (up == 1) | g_img_via_upward[i];
        }
    }
}

static void build_image_list(void) {
    g_nimg = 0;
    add_image(g_exe.base, g_exe_name[0] ? g_exe_name : g_exe.path);
    close_over_deps(0);
    LOG("images: %u loaded (exe + dependency closure)\n", g_nimg);
}

static int image_index(uint64_t mh) {
    for (uint32_t i = 0; i < g_nimg; i++) if (g_img_mh[i] == mh) return (int)i;
    return -1;
}

static void run_initializers(uint64_t mh, int argc, char **argv, char **envp, char **apple);

/* Bottom-up (post-order DFS over dylib deps) initialization, each image once. */
static void init_image_tree(uint32_t idx, int depth) {
    if (g_img_inited[idx] || depth > 64) return;
    g_img_inited[idx] = 2;   /* in progress: breaks cycles (upward links) */
    const char *dn;
    int up = 0, is_weak = 0;
    for (uint64_t ord = 1; (dn = dep_cmd_info(g_img_mh[idx], ord, &is_weak, &up)); ord++) {
        if (up) continue;   /* upward links are not "init before me" */
        int d = image_index(resolve_dylib_mh(dn, g_img_mh[idx], is_weak));
        if (d >= 0 && !g_img_inited[d]) init_image_tree((uint32_t)d, depth + 1);
    }
    LOG("init image %s\n", g_img_path[idx]);
    run_initializers(g_img_mh[idx], g_boot_argc, g_boot_argv, g_boot_envp, g_boot_apple);
    g_img_inited[idx] = 1;
}

static const char *loader_strstr(const char *haystack, const char *needle) {
    if (!needle[0]) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

static uint64_t g_objc_trampolines_mh = 0;

static uint64_t dyld_get_objc_trampolines_mh(void) {
    if (g_objc_trampolines_mh) return g_objc_trampolines_mh;

    size_t sz = 0x8000;
    void *p = sys_mmap((void *)0x320000000ull, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((uintptr_t)p >= (uintptr_t)-4095) {
        p = sys_mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((uintptr_t)p >= (uintptr_t)-4095) return 0;
    }
    memset(p, 0, sz);

    struct mach_header_64 *mh = (struct mach_header_64 *)p;
    mh->magic = 0xfeedfacf;
    mh->cputype = 0x0100000c;
    mh->cpusubtype = 0x00000002;
    mh->filetype = 0x6; /* MH_DYLIB */
    mh->ncmds = 1;
    mh->sizeofcmds = sizeof(struct segment_command_64);
    mh->flags = 0;

    struct segment_command_64 *seg = (struct segment_command_64 *)((uint8_t *)p + sizeof(struct mach_header_64));
    seg->cmd = 0x19; /* LC_SEGMENT_64 */
    seg->cmdsize = sizeof(struct segment_command_64);
    memcpy(seg->segname, "__TEXT", 7);
    seg->vmaddr = (uint64_t)p;
    seg->vmsize = 0x8000;
    seg->fileoff = 0;
    seg->filesize = 0x8000;
    seg->maxprot = 7;
    seg->initprot = 5;
    seg->nsects = 0;
    seg->flags = 0;

    /* Page 1 (offset 0x4000): trampolines */
    uint32_t *code = (uint32_t *)((uint8_t *)p + 0x4000);
    /* _objc_blockTrampolineImpl at offset 0 (0x4000) */
    code[0] = 0xf9400230;  /* ldr x16, [x17] */
    code[1] = 0xaa0003e1;  /* mov x1, x0 */
    code[2] = 0xaa1003e0;  /* mov x0, x16 */
    code[3] = 0xf9400810;  /* ldr x16, [x0, #16] */
    code[4] = 0x91004011;  /* add x17, x0, #16 */
    code[5] = 0xd71f0a11;  /* braa x16, x17 */

    /* Slots 8..2047: _objc_blockTrampolineStart is at slot 8 (offset 64 = 0x4040) */
    for (int i = 8; i < 2048; i++) {
        int idx = (i * 8) / 4;
        code[idx] = 0x10fc0011; /* adr x17, -0x8000 */
        code[idx + 1] = 0x14000000 | ((-(2 * i + 1)) & 0x03ffffff); /* b Impl */
    }

    sys3(SYS_mprotect, p, sz, PROT_READ | PROT_WRITE | PROT_EXEC);

    g_objc_trampolines_mh = (uint64_t)p;
    add_image(g_objc_trampolines_mh, "/usr/lib/libobjc-trampolines.dylib");
    int idx = image_index(g_objc_trampolines_mh);
    if (idx >= 0) g_img_inited[idx] = 1;
    LOG("synthesized /usr/lib/libobjc-trampolines.dylib at 0x%lx\n", g_objc_trampolines_mh);
    return g_objc_trampolines_mh;
}

/* dlopen of a cache image: extend the image list, notify objc, run new initializers. Returns mh or 0. */
uint64_t dyld_dlopen(const char *path) {
    if (!path) return g_exe.base;
    if (loader_strstr(path, "libobjc-trampolines")) {
        return dyld_get_objc_trampolines_mh();
    }
    uint64_t mh = dsc_image(path);
    if (!mh) { ldprintf(2, "[dyld] dlopen %s: not in shared cache\n", path); return 0; }
    int idx = image_index(mh);
    if (idx >= 0 && g_img_inited[idx] == 1) return mh;
    uint32_t first_new = g_nimg;
    if (idx < 0) { add_image(mh, dsc_image_path(mh)); close_over_deps(first_new); }
    LOG("dlopen %s: %u new images\n", path, g_nimg - first_new);
    dyld_objc_notify_new(first_new);
    for (uint32_t i = first_new; i < g_nimg; i++) init_image_tree(i, 0);
    idx = image_index(mh);
    if (idx >= 0) init_image_tree((uint32_t)idx, 0);
    return mh;
}

/* dlsym: handle = mh, or RTLD_DEFAULT (-2) / RTLD_NEXT (-1) / RTLD_SELF (-3) / RTLD_MAIN_ONLY (-5) */
uint64_t dyld_dlsym(uint64_t handle, const char *name) {
    char sym[512];
    size_t l = strlen(name);
    if (l > 500) return 0;
    sym[0] = '_';
    memcpy(sym + 1, name, l + 1);
    if (!strcmp(name, "_objc_blockTrampolineImpl") || !strcmp(sym, "__objc_blockTrampolineImpl")) {
        uint64_t base = dyld_get_objc_trampolines_mh();
        return pac_sign(base + 0x4000, 0, 0);
    }
    if (!strcmp(name, "_objc_blockTrampolineStart") || !strcmp(sym, "__objc_blockTrampolineStart")) {
        uint64_t base = dyld_get_objc_trampolines_mh();
        return pac_sign(base + 0x4040, 0, 0);
    }
    if ((int64_t)handle == -5) return dsc_lookup(g_exe.base, sym);
    if ((int64_t)handle < 0) {
        for (uint32_t i = 0; i < g_nimg; i++) { uint64_t a = dsc_lookup(g_img_mh[i], sym); if (a) return a; }
        return 0;
    }
    return dsc_lookup(handle, sym);
}

uint32_t dyld_image_count(void) { return g_nimg; }
uint64_t dyld_image_mh(uint32_t i) { return i < g_nimg ? g_img_mh[i] : 0; }
const char *dyld_image_path(uint32_t i) { return i < g_nimg ? g_img_path[i] : 0; }

/* ProgramVars as passed to libSystem_initializer */
static int g_NXArgc;
static char **g_NXArgv, **g_environ;
static const char *g_progname;
static struct { const void *mh; int *argc; char ***argv; char ***envp; const char **progname; } g_vars;

/* ---------- binds ---------- */

static uint64_t resolver(const struct image *img, int ord, const char *sym, int weak) {
    uint64_t a = 0;
    /* Thread-local variables: our own TLV thunk in libsystem_kernel (see tlv_setup). */
    if (!strcmp(sym, "__tlv_bootstrap")) {
        a = dsc_lookup(LSK_BASE, "_lsk_tlv_get_addr");
        if (!a) die("libsystem_kernel lacks _lsk_tlv_get_addr");
        return a;
    }
    if (ord > 0) {
        int is_weak_cmd = 0, up = 0;
        const char *dn = dep_cmd_info(img->base, (uint64_t)ord, &is_weak_cmd, &up);
        if (!dn) die("%s: bad dylib ordinal %d for %s", img->path, ord, sym);
        uint64_t mh = resolve_dylib_mh(dn, img->base, is_weak_cmd);
        if (!mh) {
            if (weak || is_weak_cmd) return 0;
            die("%s needs %s (not found)", img->path, dn);
        }
        a = dsc_lookup(mh, sym);
    } else if (ord == 0 || ord == -1) {
        a = dsc_lookup(img->base, sym);
    } else {   /* flat / weak lookup: search in load order across all loaded images */
        for (uint32_t i = 0; i < g_nimg; i++) {
            a = dsc_lookup(g_img_mh[i], sym);
            if (a) break;
        }
        if (!a && img) a = dsc_lookup(img->base, sym);
    }
    if (!a && !weak) die("%s: symbol %s not found (ordinal %d)", img->path, sym, ord);
    return a;
}


/* ---------- thread-local variables ----------
 * Each S_THREAD_LOCAL_VARIABLES descriptor is {thunk, key, offset}. The thunk is
 * bound to __tlv_bootstrap, which the resolver redirects to lsk_tlv_get_addr;
 * key points at a per-image record {template start, init size, total size}. */
struct tlv_info { uint64_t start, init_size, total_size; };
static struct tlv_info g_tlv_info[MAX_FRAMEWORKS + 1];
static int g_ntlv;

static void tlv_setup(const struct image *img) {
    const struct mach_header_64 *h = (const void *)img->base;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    uint64_t tstart = ~0ull, tinit_end = 0, tend = 0;
    uint64_t vstart[8], vsize[8]; int nv = 0;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const void *)cp;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)cp;
            const struct section_64 *sc = (const void *)(sg + 1);
            for (uint32_t j = 0; j < sg->nsects; j++) {
                uint32_t t = sc[j].flags & 0xff;
                uint64_t a = sc[j].addr + (uint64_t)img->slide;
                if (t == 0x11 || t == 0x12) {   /* S_THREAD_LOCAL_REGULAR / _ZEROFILL */
                    if (a < tstart) tstart = a;
                    if (a + sc[j].size > tend) tend = a + sc[j].size;
                    if (t == 0x11 && a + sc[j].size > tinit_end) tinit_end = a + sc[j].size;
                } else if (t == 0x13 && nv < 8) { /* S_THREAD_LOCAL_VARIABLES */
                    vstart[nv] = a; vsize[nv] = sc[j].size; nv++;
                }
            }
        }
        cp += lc->cmdsize;
    }
    if (!nv) return;
    if (g_ntlv >= (int)(sizeof(g_tlv_info) / sizeof(g_tlv_info[0]))) die("too many TLV images");
    struct tlv_info *ti = &g_tlv_info[g_ntlv++];
    if (tstart == ~0ull) tstart = tend = tinit_end = 0;
    ti->start = tstart;
    ti->init_size = tinit_end > tstart ? tinit_end - tstart : 0;
    ti->total_size = tend - tstart;
    uint64_t nd = 0;
    for (int v = 0; v < nv; v++) {
        uint64_t *d = (uint64_t *)vstart[v];
        for (uint64_t k = 0; k + 3 <= vsize[v] / 8; k += 3) { d[k + 1] = (uint64_t)ti; nd++; }
    }
    ldprintf(2, "[tlv] %s: %lu descriptors, template 0x%lx init %lu total %lu\n",
             img->path, nd, ti->start, ti->init_size, ti->total_size);
}

/* ---------- TLS patch (ARCHITECTURE.md §7) ---------- */

static void apply_tls_sites(const char *path) {
    long fd = sys4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) die("open %s: %ld (build it with `make tls-sites` and push)", path, fd);
    uint8_t st[128];
    sys2(SYS_fstat, fd, st);
    uint64_t size = *(uint64_t *)(st + 48);
    const uint8_t *f = sys_mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
    sys1(SYS_close, fd);
    if (MAP_FAILED_P(f) || size < 32 || memcmp(f, "IOSWTLS1", 8)) die("%s: bad TLS site file", path);
    /* f+8: cache UUID (checked against header offset 0x58) */
    if (memcmp(f + 8, (const void *)(0x180000000ull + 0x58), 16)) die("%s: built for a different cache", path);
    uint64_t n = *(const uint64_t *)(f + 24);
    const uint64_t *e = (const uint64_t *)(f + 32);
    if (32 + n * 8 > size) die("%s truncated", path);
    uint64_t done = 0, pages = 0, i = 0;
    while (i < n) {
        uint64_t page = (e[i] & 0x00ffffffffffffffull) & ~(g_pagesize - 1), j = i;
        while (j < n && ((e[j] & 0x00ffffffffffffffull) & ~(g_pagesize - 1)) == page) j++;
        text_make_writable(page, page + g_pagesize);
        for (; i < j; i++) {
            uint32_t *ins = (uint32_t *)(e[i] & 0x00ffffffffffffffull);
            unsigned kind = e[i] >> 56, rd = *ins & 31;
            if (kind == 1 && (*ins & 0xffffffe0) == 0xd53bd060) *ins = 0xd53bd040 | rd;      /* -> mrs xN, tpidr_el0 */
            else if (kind == 2 && (*ins & 0xffffffe0) == 0xd53bd040) *ins = 0xd2800000 | rd; /* -> movz xN, #0 (cpu 0) */
            else die("TLS site 0x%lx: unexpected instruction 0x%x (kind %u)", (uint64_t)ins, *ins, kind);
            done++;
        }
        text_seal(page, page + g_pagesize);
        pages++;
    }
    sys2(SYS_munmap, f, size);
    LOG("tls: %lu sites patched on %lu pages\n", done, pages);
}

/* ---------- initializers ---------- */

typedef void (*init_fn)(int, char **, char **, char **, void *);

static void run_initializers(uint64_t mh, int argc, char **argv, char **envp, char **apple) {
    const struct mach_header_64 *h = (const void *)mh;
    const uint8_t *cp = (const uint8_t *)(h + 1);
    int64_t slide = 0;
    for (uint32_t i = 0; i < h->ncmds; i++, cp += ((const struct load_command *)cp)->cmdsize) {
        const struct load_command *lc = (const void *)cp;
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sc = (const void *)lc;
        if (!strcmp(sc->segname, "__TEXT")) slide = (int64_t)(mh - sc->vmaddr);
        const struct section_64 *s = (const void *)(sc + 1);
        for (uint32_t k = 0; k < sc->nsects; k++) {
            uint32_t type = s[k].flags & 0xff;
            if (type == 0x16) {                /* S_INIT_FUNC_OFFSETS: u32 offsets from mh */
                const uint32_t *o = (const uint32_t *)(s[k].addr + slide);
                for (uint64_t j = 0; j < s[k].size / 4; j++) {
                    LOG("init 0x%lx (offset 0x%x)\n", mh + o[j], o[j]);
                    ((init_fn)(mh + o[j]))(argc, argv, envp, apple, &g_vars);
                }
            } else if (type == 0x9) {          /* S_MOD_INIT_FUNC_POINTERS */
                const uint64_t *p = (const uint64_t *)(s[k].addr + slide);
                for (uint64_t j = 0; j < s[k].size / 8; j++) {
                    LOG("init ptr 0x%lx\n", p[j]);
                    ((init_fn)p[j])(argc, argv, envp, apple, &g_vars);
                }
            }
        }
    }
}

/* Runs on the Darwin stack (entered via call_main_on_stack). */
__attribute__((noreturn)) static void darwin_boot(int argc, char **argv, char **envp, char **apple) {
    g_NXArgc = argc;
    g_NXArgv = argv;
    g_environ = envp;
    g_progname = argc ? argv[0] : "";
    for (const char *p = g_progname; *p; p++) if (*p == '/') g_progname = p + 1;
    g_vars = (typeof(g_vars)){ (const void *)g_exe.base, &g_NXArgc, &g_NXArgv, &g_environ, &g_progname };

    g_boot_argc = argc; g_boot_argv = argv; g_boot_envp = envp; g_boot_apple = apple;
    {   /* libdyld's exported crt variables */
        uint64_t ld = dsc_image("/usr/lib/system/libdyld.dylib"), v;
        if (ld && (v = dsc_lookup(ld, "_NXArgc"))) *(int *)v = argc;
        if (ld && (v = dsc_lookup(ld, "_NXArgv"))) *(char ***)v = argv;
        if (ld && (v = dsc_lookup(ld, "_environ"))) *(char ***)v = envp;
        if (ld && (v = dsc_lookup(ld, "___progname"))) *(const char **)v = g_progname;
    }
    LOG("boot: libSystem initializers\n");
    int ls = image_index(g_libsystem);
    if (ls >= 0) g_img_inited[ls] = 2;
    run_initializers(g_libsystem, argc, argv, envp, apple);
    if (ls >= 0) g_img_inited[ls] = 1;
    dyld_objc_run_inits();
    LOG("boot: remaining initializers (bottom-up)\n");
    for (uint32_t i = 1; i < g_nimg; i++) init_image_tree(i, 0);
    LOG("boot: exe initializers\n");
    g_img_inited[0] = 1;
    run_initializers(g_exe.base, argc, argv, envp, apple);
    LOG("boot: main 0x%lx\n", g_exe.entry);
    int r = ((int (*)(int, char **, char **, char **))g_exe.entry)(argc, argv, envp, apple);
    uint64_t ex = dsc_lookup(g_libsystem, "_exit");
    LOG("boot: main returned %d, exit 0x%lx\n", r, ex);
    if (ex) ((void (*)(int))ex)(r);
    sys1(SYS_exit_group, r);
    __builtin_unreachable();
}

/* ---------- setup on the loader stack ---------- */

uint64_t dyld_setup(const char *exe_path, const char *dsc_path, char **envp, uint64_t *boot_fn) {
    const char *lsk_path = 0, *tls_path = 0;
    for (char **e = envp; *e; e++) {
        if (!strncmp(*e, "IOSWINE_LSK=", 12)) lsk_path = *e + 12;
        if (!strncmp(*e, "IOSWINE_TLS=", 12)) tls_path = *e + 12;
    }
    if (!lsk_path) lsk_path = "./libsystem_kernel.dylib";
    if (!tls_path) tls_path = "./tls_sites.bin";

    /* PAC mode: arm64 executables run with user keys disabled (PAC instructions become NOPs), as XNU does
       for non-arm64e tasks. Must precede cache signing. */
    {
        long fd = sys4(SYS_openat, AT_FDCWD, exe_path, O_RDONLY, 0);
        struct mach_header_64 mh;
        if (fd < 0 || sys3(SYS_read, fd, &mh, sizeof mh) != sizeof mh) die("read %s", exe_path);
        sys1(SYS_close, fd);
        int arm64e = (mh.cpusubtype & CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E;
        if (!arm64e) {
            long r = sys5(SYS_prctl, PR_PAC_SET_ENABLED_KEYS, 0xf, 0, 0, 0);
            /* qemu-user lacks this prctl: keep keys on and run as arm64e (host crash-repro only). */
            if (r < 0) { LOG("pac: PR_PAC_SET_ENABLED_KEYS unsupported (%ld), keys stay enabled\n", r); arm64e = 1; }
        }
        LOG("pac: %s executable, user keys %s\n", arm64e ? "arm64e" : "arm64", arm64e ? "enabled" : "disabled");
    }

    dsc_load(dsc_path);
    g_libsystem = dsc_image("/usr/lib/libSystem.B.dylib");
    if (!g_libsystem) die("no libSystem.B in cache");
    apply_tls_sites(tls_path);
    commpage_setup();

    macho_load(lsk_path, (int64_t)LSK_BASE, &g_lsk);
    lsk_swap(g_lsk.base, 0);
    for (int i = 0; i < g_lsk.nsegs; i++)
        if (g_lsk.segs[i].prot & PROT_EXEC) sigsys_allow_range(g_lsk.segs[i].addr, g_lsk.segs[i].addr + g_lsk.segs[i].size);

    macho_bind_resolver = resolver;
    set_app_dir_from_exe(exe_path);
    macho_map(exe_path, 0, &g_exe);
    if (g_exe.entry_kind != ENTRY_MAIN) die("libSystem mode needs an LC_MAIN executable");

    build_image_list();

    /* Apply fixups in dependency order: embedded frameworks first, then main executable */
    for (int k = g_nframeworks - 1; k >= 0; k--) {
        macho_fixup(&g_frameworks[k]);
        tlv_setup(&g_frameworks[k]);
    }
    macho_fixup(&g_exe);
    tlv_setup(&g_exe);

    dyld_apis_setup();

    /* temporary TSD until libpthread installs the main thread's: page aligned so the patched
       cpu-number readers (and any leftover low-bit users) see 0 */
    uint64_t *tsd = sys_mmap(0, g_pagesize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (MAP_FAILED_P(tsd)) die("tsd mmap");
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(tsd));

    *boot_fn = (uint64_t)darwin_boot;
    return g_exe.entry;
}

static void find_exe_in_target(const char *target, char *out_exe, size_t out_sz) {
    long fd = sys4(SYS_openat, AT_FDCWD, target, O_RDONLY, 0);
    if (fd >= 0) {
        uint8_t st[128];
        if (sys2(SYS_fstat, fd, st) == 0) {
            uint32_t mode = *(uint32_t *)(st + 24);
            if ((mode & 0170000) != 0040000) {
                sys1(SYS_close, fd);
                size_t l = strlen(target);
                if (l < out_sz) { memcpy(out_exe, target, l + 1); return; }
            }
        }
        sys1(SYS_close, fd);
    }

    char plist_path[512];
    size_t tl = strlen(target);
    while (tl > 0 && target[tl - 1] == '/') tl--;
    if (tl < sizeof(plist_path) - 15) {
        memcpy(plist_path, target, tl);
        memcpy(plist_path + tl, "/Info.plist", 12);
        long pfd = sys4(SYS_openat, AT_FDCWD, plist_path, O_RDONLY, 0);
        if (pfd >= 0) {
            char buf[4096];
            long r = sys3(SYS_read, pfd, buf, sizeof(buf) - 1);
            sys1(SYS_close, pfd);
            if (r > 0) {
                buf[r] = '\0';
                const char *key = "<key>CFBundleExecutable</key>";
                char *k = buf;
                while (*k) {
                    if (!strncmp(k, key, 29)) {
                        char *s = k + 29;
                        while (*s && strncmp(s, "<string>", 8)) s++;
                        if (*s) {
                            s += 8;
                            char *e = s;
                            while (*e && strncmp(e, "</string>", 9)) e++;
                            if (*e) {
                                *e = '\0';
                                size_t nlen = strlen(s);
                                if (tl + 1 + nlen < out_sz) {
                                    memcpy(out_exe, target, tl);
                                    out_exe[tl] = '/';
                                    memcpy(out_exe + tl + 1, s, nlen + 1);
                                    return;
                                }
                            }
                        }
                        break;
                    }
                    k++;
                }
            }
        }
    }

    const char *slash = 0;
    for (const char *p = target; *p; p++) if (*p == '/') slash = p;
    const char *base = slash ? slash + 1 : target;
    char bname[128];
    size_t bl = strlen(base);
    if (bl > 4 && !strcmp(base + bl - 4, ".app")) bl -= 4;
    if (bl < sizeof(bname)) {
        memcpy(bname, base, bl);
        bname[bl] = '\0';
        if (tl + 1 + bl < out_sz) {
            memcpy(out_exe, target, tl);
            out_exe[tl] = '/';
            memcpy(out_exe + tl + 1, bname, bl + 1);
            return;
        }
    }
    size_t l = strlen(target);
    if (l < out_sz) memcpy(out_exe, target, l + 1);
}

void dyld_print_plan(const char *target, const char *dsc_path, char **envp) {
    (void)envp;
    dsc_load(dsc_path);
    g_libsystem = dsc_image("/usr/lib/libSystem.B.dylib");
    if (!g_libsystem) die("no libSystem.B in cache");

    char actual_exe[512];
    find_exe_in_target(target, actual_exe, sizeof(actual_exe));
    set_app_dir_from_exe(actual_exe);

    macho_bind_resolver = resolver;
    macho_map(actual_exe, 0, &g_exe);
    build_image_list();

    ldprintf(1, "=== Load Plan ===\n");
    ldprintf(1, "Images (%u):\n", g_nimg);
    for (uint32_t i = 0; i < g_nimg; i++) {
        const char *name = g_img_path[i];
        if (i == 0) name = g_exe_name[0] ? g_exe_name : g_exe.path;
        ldprintf(1, "  [%4u] %s\n", i, name);
    }
    ldprintf(1, "\n");

    const struct dsc_alias_entry *aliases = 0;
    int naliases = 0;
    dsc_get_aliases(&aliases, &naliases);
    ldprintf(1, "Aliases taken (%d):\n", naliases);
    for (int i = 0; i < naliases; i++) {
        ldprintf(1, "  %s -> %s\n", aliases[i].from, aliases[i].to);
    }
    ldprintf(1, "\n");

    ldprintf(1, "@rpath resolution (%d):\n", g_nrpath_res);
    for (int i = 0; i < g_nrpath_res; i++) {
        ldprintf(1, "  %s -> %s\n", g_rpath_res[i].from, g_rpath_res[i].to);
    }
    ldprintf(1, "\n");

    uint32_t total_binds = 0;
    ldprintf(1, "Binds per kind:\n");

    if (g_exe.n_chained_imports || g_exe.n_bind || g_exe.n_lazy || g_exe.n_weak) {
        ldprintf(1, "  %s:\n", g_exe_name[0] ? g_exe_name : "main");
        if (g_exe.n_chained_imports) {
            ldprintf(1, "    chained: %u\n", g_exe.n_chained_imports);
            total_binds += g_exe.n_chained_imports;
        }
        if (g_exe.n_bind) {
            ldprintf(1, "    bind: %u\n", g_exe.n_bind);
            total_binds += g_exe.n_bind;
        }
        if (g_exe.n_lazy) {
            ldprintf(1, "    lazy: %u\n", g_exe.n_lazy);
            total_binds += g_exe.n_lazy;
        }
        if (g_exe.n_weak) {
            ldprintf(1, "    weak: %u\n", g_exe.n_weak);
            total_binds += g_exe.n_weak;
        }
    }

    for (int k = 0; k < g_nframeworks; k++) {
        struct image *fw = &g_frameworks[k];
        if (fw->n_chained_imports || fw->n_bind || fw->n_lazy || fw->n_weak) {
            const char *fn = g_framework_paths[k];
            const char *slash = 0;
            for (const char *p = fn; *p; p++) if (*p == '/') slash = p;
            const char *base = slash ? slash + 1 : fn;
            ldprintf(1, "  %s:\n", base);
            if (fw->n_chained_imports) {
                ldprintf(1, "    chained: %u\n", fw->n_chained_imports);
                total_binds += fw->n_chained_imports;
            }
            if (fw->n_bind) {
                ldprintf(1, "    bind: %u\n", fw->n_bind);
                total_binds += fw->n_bind;
            }
            if (fw->n_lazy) {
                ldprintf(1, "    lazy: %u\n", fw->n_lazy);
                total_binds += fw->n_lazy;
            }
            if (fw->n_weak) {
                ldprintf(1, "    weak: %u\n", fw->n_weak);
                total_binds += fw->n_weak;
            }
        }
    }
    ldprintf(1, "  Total binds: %u\n\n", total_binds);
}

uint64_t dyld_exe_mh(void) { return g_exe.base; }
const char *dyld_exe_path(void) { return g_exe.path; }
