/* ioswine loader entry: parse Linux stack, load Mach-O, build Darwin stack, hand off
   (ARCHITECTURE.md §1, §2, §4). */
#include "macho.h"
#include "rt.h"
#include "dsc.h"
#include "dyld.h"
#include "commpage.h"
#include "sigsys.h"
#include "sys.h"

#define AT_NULL   0
#define AT_PAGESZ 6
#define DEFAULT_STACK (8ull << 20)

uint64_t g_pagesize = 4096;

extern const char __ehdr_start[];
extern const char etext[];

/* entry(a0..a3) on stack sp; exit_group(return value). */
__attribute__((noreturn)) void call_main_on_stack(uint64_t entry, uint64_t sp, long a0, long a1, long a2, long a3);
__asm__(".text\n.p2align 2\n.globl call_main_on_stack\ncall_main_on_stack:\n"
        "\tmov sp, x1\n\tmov x16, x0\n\tmov x0, x2\n\tmov x1, x3\n\tmov x2, x4\n\tmov x3, x5\n"
        "\tmov x29, xzr\n\tblr x16\n\tmov x8, #94\n\tsvc #0\n\tbrk #1\n");
/* Kernel-style start: sp -> argc, jump to pc with zeroed fp/lr. */
__attribute__((noreturn)) void jump_unixthread(uint64_t pc, uint64_t sp);
__asm__(".text\n.p2align 2\n.globl jump_unixthread\njump_unixthread:\n"
        "\tmov sp, x1\n\tmov x29, xzr\n\tmov x30, xzr\n\tmov x16, x0\n\tmov x0, xzr\n\tmov x1, xzr\n"
        "\tbr x16\n");

static const char *getenv_(char **envp, const char *k) {
    size_t l = strlen(k);
    for (; *envp; envp++)
        if (!strncmp(*envp, k, l) && (*envp)[l] == '=') return *envp + l + 1;
    return 0;
}

static uint64_t bplist_read_uint(const uint8_t *p, int sz) {
    uint64_t v = 0;
    for (int i = 0; i < sz; i++) v = (v << 8) | p[i];
    return v;
}

static int bplist_parse_obj(const uint8_t *b, uint64_t psz, uint64_t off,
                            uint8_t *out_tag, uint64_t *out_count, uint64_t *out_p) {
    if (off >= psz) return -1;
    uint8_t marker = b[off];
    uint8_t tag = marker >> 4;
    uint64_t count = marker & 0x0f;
    uint64_t p = off + 1;
    if (count == 0x0f) {
        if (p >= psz) return -1;
        uint8_t ib = b[p++];
        int cnt_sz = 1 << (ib & 0x0f);
        if (p + cnt_sz > psz) return -1;
        count = bplist_read_uint(b + p, cnt_sz);
        p += cnt_sz;
    }
    *out_tag = tag;
    *out_count = count;
    *out_p = p;
    return 0;
}

static int parse_bplist_executable(const uint8_t *b, uint64_t psz, char *exe_out, size_t exe_sz) {
    if (psz < 40 || memcmp(b, "bplist00", 8) != 0) return -1;
    const uint8_t *tr = b + psz - 32;
    uint8_t off_sz = tr[6];
    uint8_t ref_sz = tr[7];
    if (off_sz == 0 || off_sz > 8 || ref_sz == 0 || ref_sz > 8) return -1;
    uint64_t num_objs = bplist_read_uint(tr + 8, 8);
    uint64_t top_obj = bplist_read_uint(tr + 16, 8);
    uint64_t off_table = bplist_read_uint(tr + 24, 8);

    if (top_obj >= num_objs) return -1;
    if (off_table + num_objs * off_sz > psz) return -1;

    uint64_t top_off = bplist_read_uint(b + off_table + top_obj * off_sz, off_sz);
    uint8_t top_tag;
    uint64_t top_count, top_p;
    if (bplist_parse_obj(b, psz, top_off, &top_tag, &top_count, &top_p) != 0) return -1;
    if (top_tag != 0xD) return -1;

    uint64_t keys_p = top_p;
    uint64_t vals_p = top_p + top_count * ref_sz;
    if (vals_p + top_count * ref_sz > psz) return -1;

    for (uint64_t i = 0; i < top_count; i++) {
        uint64_t k_ref = bplist_read_uint(b + keys_p + i * ref_sz, ref_sz);
        if (k_ref >= num_objs) continue;
        uint64_t k_off = bplist_read_uint(b + off_table + k_ref * off_sz, off_sz);
        uint8_t k_tag;
        uint64_t k_len, k_p;
        if (bplist_parse_obj(b, psz, k_off, &k_tag, &k_len, &k_p) != 0) continue;
        if (k_tag == 5 && k_len == 18 && k_p + 18 <= psz && !memcmp(b + k_p, "CFBundleExecutable", 18)) {
            uint64_t v_ref = bplist_read_uint(b + vals_p + i * ref_sz, ref_sz);
            if (v_ref >= num_objs) return -1;
            uint64_t v_off = bplist_read_uint(b + off_table + v_ref * off_sz, off_sz);
            uint8_t v_tag;
            uint64_t v_len, v_p;
            if (bplist_parse_obj(b, psz, v_off, &v_tag, &v_len, &v_p) != 0) return -1;
            if (v_tag == 5 && v_len > 0 && v_len < exe_sz && v_p + v_len <= psz) {
                memcpy(exe_out, b + v_p, v_len);
                exe_out[v_len] = '\0';
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

static int parse_xml_plist_executable(const char *s, size_t psz, char *exe_out, size_t exe_sz) {
    const char *end = s + psz;
    const char *key = "<key>CFBundleExecutable</key>";
    while (s + 29 < end) {
        if (!memcmp(s, key, 29)) {
            s += 29;
            while (s + 8 < end && memcmp(s, "<string>", 8) != 0) s++;
            if (s + 8 < end) {
                s += 8;
                const char *v_start = s;
                while (s + 9 <= end && memcmp(s, "</string>", 9) != 0) s++;
                if (s + 9 <= end) {
                    size_t v_len = (size_t)(s - v_start);
                    if (v_len > 0 && v_len < exe_sz) {
                        memcpy(exe_out, v_start, v_len);
                        exe_out[v_len] = '\0';
                        return 0;
                    }
                }
            }
            return -1;
        }
        s++;
    }
    return -1;
}

static const char *resolve_app_bundle(const char *path) {
    if (!path) return path;
    static char resolved[1024];
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    if (len < 4) return path;
    if (memcmp(path + len - 4, ".app", 4) != 0) return path;

    char clean_path[512];
    if (len >= sizeof(clean_path)) return path;
    memcpy(clean_path, path, len);
    clean_path[len] = '\0';

    long fd = sys4(SYS_openat, AT_FDCWD, clean_path, O_RDONLY, 0);
    if (fd < 0) return path;
    uint8_t st[128];
    if (sys2(SYS_fstat, fd, st) < 0) {
        sys1(SYS_close, fd);
        return path;
    }
    sys1(SYS_close, fd);
    uint32_t mode = *(uint32_t *)(st + 16);
    if ((mode & 0170000) != 0040000) return path; /* not a directory */

    char exe_name[256];
    int found = 0;

    /* Try Info.plist */
    char plist_path[512];
    if (len + 12 < sizeof(plist_path)) {
        memcpy(plist_path, clean_path, len);
        memcpy(plist_path + len, "/Info.plist", 12);
        long pfd = sys4(SYS_openat, AT_FDCWD, plist_path, O_RDONLY, 0);
        if (pfd >= 0) {
            if (sys2(SYS_fstat, pfd, st) == 0) {
                uint64_t psz = *(uint64_t *)(st + 48);
                if (psz > 8 && psz <= 2 * 1024 * 1024) {
                    void *pdata = sys_mmap(0, psz, PROT_READ, MAP_PRIVATE, pfd, 0);
                    if (!MAP_FAILED_P(pdata)) {
                        if (parse_bplist_executable(pdata, psz, exe_name, sizeof(exe_name)) == 0) {
                            found = 1;
                        } else if (parse_xml_plist_executable((const char *)pdata, (size_t)psz, exe_name, sizeof(exe_name)) == 0) {
                            found = 1;
                        }
                        sys2(SYS_munmap, pdata, psz);
                    }
                }
            }
            sys1(SYS_close, pfd);
        }
    }

    if (!found) {
        /* Fallback: <basename without .app> */
        const char *base = clean_path;
        for (const char *p = clean_path; *p; p++) if (*p == '/') base = p + 1;
        size_t blen = strlen(base);
        if (blen > 4 && !strcmp(base + blen - 4, ".app")) blen -= 4;
        if (blen > 0 && blen < sizeof(exe_name)) {
            memcpy(exe_name, base, blen);
            exe_name[blen] = '\0';
            found = 1;
        }
    }

    if (found) {
        size_t elen = strlen(exe_name);
        if (len + 1 + elen < sizeof(resolved)) {
            memcpy(resolved, clean_path, len);
            resolved[len] = '/';
            memcpy(resolved + len + 1, exe_name, elen + 1);
            return resolved;
        }
    }
    return path;
}

__attribute__((noreturn)) static void dsc_selftest(char **envp) {
    const char *path = getenv_(envp, "IOSWINE_DSC");
    if (!path) die("set IOSWINE_DSC=<main cache file>");
    dsc_load(path);
    ldprintf(1, "dsc: rebased %lu plain, %lu auth\n", dsc_stat_plain, dsc_stat_auth);
    int bad = 0;
    for (int i = 0; i < dsc_nsamples; i++) {
        struct dsc_auth_sample *s = &dsc_samples[i];
        uint64_t v = *(uint64_t *)s->loc, a = pac_auth(v, s->key, s->disc);
        if (a != s->target) { bad++; ldprintf(1, "dsc: auth FAIL loc 0x%lx val 0x%lx -> 0x%lx want 0x%lx\n", s->loc, v, a, s->target); }
    }
    ldprintf(1, "dsc: %d/%d signed pointers authenticate\n", dsc_nsamples - bad, dsc_nsamples);
    uint64_t plat = dsc_image("/usr/lib/system/libsystem_platform.dylib");
    uint64_t sysb = dsc_image("/usr/lib/libSystem.B.dylib");
    ldprintf(1, "dsc: libsystem_platform @0x%lx libSystem.B @0x%lx\n", plat, sysb);
    uint64_t f1 = plat ? dsc_lookup(plat, "__platform_strlen") : 0;
    uint64_t f2 = sysb ? dsc_lookup(sysb, "_strlen") : 0;
    uint64_t f3 = sysb ? dsc_lookup(sysb, "_write") : 0;
    ldprintf(1, "dsc: _platform_strlen=0x%lx strlen(via libSystem)=0x%lx write=0x%lx\n", f1, f2, f3);
    if (f1) ldprintf(1, "dsc: platform_strlen(\"hello, cache\") = %lu\n", ((size_t (*)(const char *))f1)("hello, cache"));
    if (f2) ldprintf(1, "dsc: strlen(\"abc\") = %lu\n", ((size_t (*)(const char *))f2)("abc"));
    ldprintf(1, bad || !f1 || !f2 ? "DSC SELFTEST FAIL\n" : "DSC SELFTEST OK\n");
    sys1(SYS_exit_group, bad || !f1 || !f2);
    __builtin_unreachable();
}

static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* Build [argc][argv..][0][envp..][0][apple..][0] + strings at the top of a fresh stack.
   Returns the address of argc (16-byte aligned). */
static uint64_t build_darwin_stack(uint64_t top, int argc, char **argv, char **envp, char **apple, int napple,
                                   char ***out_argv, char ***out_envp, char ***out_apple) {
    int nenv = 0;
    while (envp[nenv]) nenv++;
    /* copy strings downwards */
    char *sp = (char *)top;
    char *sargv[64], *senv[256], *sapple[8];
    if (argc > 64 || nenv > 256 || napple > 8) die("too many args/env");
#define PUSH_STR(dst, s) do { size_t l = strlen(s) + 1; sp -= l; memcpy(sp, s, l); dst = sp; } while (0)
    for (int i = 0; i < napple; i++) PUSH_STR(sapple[i], apple[i]);
    for (int i = 0; i < nenv; i++) PUSH_STR(senv[i], envp[i]);
    for (int i = 0; i < argc; i++) PUSH_STR(sargv[i], argv[i]);
    uint64_t words = 1 + (argc + 1) + (nenv + 1) + (napple + 1);
    uint64_t *v = (uint64_t *)(((uint64_t)sp - words * 8) & ~15ull);
    uint64_t *w = v;
    *w++ = argc;
    *out_argv = (char **)w;
    for (int i = 0; i < argc; i++) *w++ = (uint64_t)sargv[i];
    *w++ = 0;
    *out_envp = (char **)w;
    for (int i = 0; i < nenv; i++) *w++ = (uint64_t)senv[i];
    *w++ = 0;
    *out_apple = (char **)w;
    for (int i = 0; i < napple; i++) *w++ = (uint64_t)sapple[i];
    *w++ = 0;
    return (uint64_t)v;
}

__attribute__((noreturn)) void loader_main(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    char **e = envp;
    while (*e) e++;
    for (uint64_t *av = (uint64_t *)(e + 1); av[0] != AT_NULL; av += 2)
        if (av[0] == AT_PAGESZ) g_pagesize = av[1];
    for (e = envp; *e; e++)
        if (starts_with(*e, "IOSWINE_LOG=1")) rt_log_enabled = 1;

    if (argc >= 2 && !strcmp(argv[1], "--dsc-selftest")) dsc_selftest(envp);
    if (argc >= 2 && !strcmp(argv[1], "--pac-keys-test")) {
        long g = sys5(SYS_prctl, 61 /* PR_PAC_GET_ENABLED_KEYS */, 0, 0, 0, 0);
        ldprintf(1, "pac: enabled keys before = %ld\n", g);
        long r = sys5(SYS_prctl, 60 /* PR_PAC_SET_ENABLED_KEYS */, 0xf, 0, 0, 0);
        ldprintf(1, "pac: disable all -> %ld, now = %ld\n", r, sys5(SYS_prctl, 61, 0, 0, 0, 0));
        uint64_t v = pac_sign(0x123456789ull, 0, 0x42);
        ldprintf(1, "pac: pacia(0x123456789) = 0x%lx (%s)\n", v, v == 0x123456789ull ? "NOP" : "signed");
        sys1(SYS_exit_group, 0);
    }
    if (argc >= 2 && !strcmp(argv[1], "--execmem-test")) {
        uint32_t *c = sys_mmap(0, g_pagesize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        c[0] = 0xd2800540; c[1] = 0xd65f03c0;            /* mov x0, #42 ; ret */
        long r = sys3(SYS_mprotect, c, g_pagesize, PROT_READ | PROT_EXEC);
        if (r < 0) { ldprintf(1, "execmem: mprotect RX failed %ld\n", r); sys1(SYS_exit_group, 1); }
        flush_icache((uint64_t)c, (uint64_t)c + 8);
        ldprintf(1, "execmem: anon RW->RX ok, call = %ld\n", ((long (*)(void))c)());
        sys1(SYS_exit_group, 0);
    }
    if (argc >= 2 && !strcmp(argv[1], "--loader-plan")) {
        if (argc < 3) {
            ldprintf(2, "usage: %s --loader-plan <macho executable or .app dir>\n", argv[0]);
            sys1(SYS_exit_group, 2);
        }
        const char *target = resolve_app_bundle(argv[2]);
        const char *dsc = getenv_(envp, "IOSWINE_DSC");
        if (!dsc) dsc = "vendor-local/23G90__iPhone17,3/dyld_shared_cache_arm64e";
        dyld_print_plan(target, dsc, envp);
        sys1(SYS_exit_group, 0);
    }
    if (argc < 2) {
        ldprintf(2, "usage: %s <mach-o executable> [args...]\n", argv[0]);
        sys1(SYS_exit_group, 2);
    }
    argv[1] = (char *)resolve_app_bundle(argv[1]);
    const char *path = argv[1];
    LOG("pagesize %lu, loading %s\n", g_pagesize, path);

    /* If the sysroot lacks Apple's FeatureFlags plists (e.g. slim sysroot)
     * and the caller has not overridden FeatureFlags, enable SwiftUI/Solarium
     * so SDK 26+ apps take the iOS 26 Liquid Glass path instead of falling back
     * to legacy iOS 18 styling when /System/Library/FeatureFlags is missing. */
    char *ff_envp[258];
    if (!getenv_(envp, "FEATUREFLAGS_ENABLED") && !getenv_(envp, "FEATUREFLAGS_DISABLED")) {
        const char *sr = getenv_(envp, "IOSWINE_SYSROOT");
        if (!sr) sr = "/data/local/tmp/ioswine/root";
        size_t sl = strlen(sr);
        static const char ff_rel[] = "/System/Library/FeatureFlags/Domain/SwiftUI.plist";
        if (sl + sizeof(ff_rel) <= 600) {
            char ffp[600];
            memcpy(ffp, sr, sl);
            memcpy(ffp + sl, ff_rel, sizeof(ff_rel));
            long fd = sys4(SYS_openat, AT_FDCWD, (long)ffp, O_RDONLY, 0);
            if (fd >= 0) {
                sys1(SYS_close, fd);
            } else {
                int ne = 0;
                while (envp[ne] && ne < 254) { ff_envp[ne] = envp[ne]; ne++; }
                ff_envp[ne++] = "FEATUREFLAGS_ENABLED=SwiftUI/Solarium";
                ff_envp[ne] = 0;
                envp = ff_envp;
            }
        }
    }

    const char *dsc = getenv_(envp, "IOSWINE_DSC");
    struct image img;
    uint64_t boot_fn = 0;
    if (dsc) {
        memset(&img, 0, sizeof img);
        img.entry_kind = ENTRY_MAIN;
        img.entry = dyld_setup(path, dsc, envp, &boot_fn);
    } else {
        macho_load(path, 0, &img);
    }

    uint64_t ssize = img.stacksize ? (img.stacksize + g_pagesize - 1) & ~(g_pagesize - 1) : DEFAULT_STACK;
    uint8_t *stk = sys_mmap(0, ssize + g_pagesize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (MAP_FAILED_P(stk)) die("stack mmap");
    sys3(SYS_mprotect, stk, g_pagesize, PROT_NONE);          /* guard page */
    uint64_t top = (uint64_t)stk + g_pagesize + ssize;

    char exe[600];
    size_t pl = strlen(path);
    if (pl > 500) die("path too long");
    memcpy(exe, "executable_path=", 16);
    memcpy(exe + 16, path, pl + 1);
    char *apple[] = { exe };

    char **dargv, **denvp, **dapple;
    uint64_t dsp = build_darwin_stack(top, argc - 1, argv + 1, envp, apple, 1, &dargv, &denvp, &dapple);

    sigsys_allow_range((uint64_t)__ehdr_start, (uint64_t)etext);
    sigsys_install();

    LOG("handoff -> 0x%lx sp 0x%lx\n", img.entry, dsp);
    if (boot_fn)
        call_main_on_stack(boot_fn, dsp, argc - 1, (long)dargv, (long)denvp, (long)dapple);
    if (img.entry_kind == ENTRY_MAIN)
        call_main_on_stack(img.entry, dsp, argc - 1, (long)dargv, (long)denvp, (long)dapple);
    jump_unixthread(img.entry, dsp);
}
