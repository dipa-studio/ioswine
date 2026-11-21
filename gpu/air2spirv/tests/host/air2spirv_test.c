/* air2spirv_test.c - host unit test for the freestanding SPIR-V emitter.
 *
 * Plain cc, no NDK. The library itself uses no libc; this harness does
 * (stdio, stdlib, string) only to drive it and to report results.
 * Emits the trivial vertex+fragment pair, checks the module structure
 * word by word, writes the .spv files out, and validates them with
 * spirv-val when the tool is present (the run script enforces it).
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -O1 -I../../src \
 *      -o air2spirv_test air2spirv_test.c ../../src/air2spirv.c
 *   ./air2spirv_test [output_dir]
 *
 * Sanitizers:
 *   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
 *      -fno-sanitize-recover=all -I../../src \
 *      -o air2spirv_test_asan air2spirv_test.c ../../src/air2spirv.c
 *   ./air2spirv_test_asan [output_dir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "air2spirv.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_ERR(call, want) do { \
    int _rc = (call); \
    g_checks++; \
    if (_rc != (want)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s returned %d (%s), want %d\n", \
               __FILE__, __LINE__, #call, _rc, a2s_error_string(_rc), \
               (want)); \
    } \
} while (0)

static uint32_t g_words[512];
static uint32_t g_count;

/* Scan results for one module. */
typedef struct {
    int entry_model;        /* -1 if no OpEntryPoint */
    char entry_name[64];
    int has_origin_upper_left;
    int has_builtin_position;
    int has_location_0;
    int has_load;
    int has_store;
    int has_return;
    int has_function_end;
    int const_count;
    uint32_t const_values[16];
    int composite_count;
    uint32_t composite_parts[8];
} scan_t;

static void scan_module(const uint32_t *w, uint32_t count, scan_t *s)
{
    uint32_t pos = 5; /* past the header */
    memset(s, 0, sizeof(*s));
    s->entry_model = -1;
    while (pos < count) {
        uint32_t word = w[pos];
        uint32_t wc = word >> 16;
        uint32_t op = word & 0xffff;
        if (wc == 0 || pos + wc > count)
            break; /* malformed; the header checks catch it */
        if (op == 15 && wc >= 3) { /* OpEntryPoint */
            uint32_t i;
            size_t k = 0;
            int done = 0;
            s->entry_model = (int)w[pos + 1];
            /* Name starts at pos+3, NUL-terminated, word padded. */
            for (i = 3; i < wc && k < sizeof(s->entry_name) - 1 && !done;
                 i++) {
                uint32_t nw = w[pos + i];
                int b;
                for (b = 0; b < 4; b++) {
                    char c = (char)((nw >> (8 * b)) & 0xff);
                    if (c == '\0') {
                        done = 1;
                        break;
                    }
                    s->entry_name[k++] = c;
                }
            }
            s->entry_name[k] = '\0';
        } else if (op == 16 && wc >= 3) { /* OpExecutionMode */
            if (w[pos + 2] == 7)
                s->has_origin_upper_left = 1;
        } else if (op == 71 && wc >= 4) { /* OpDecorate */
            if (w[pos + 2] == 11 && w[pos + 3] == 0)
                s->has_builtin_position = 1;
            if (w[pos + 2] == 30 && w[pos + 3] == 0)
                s->has_location_0 = 1;
        } else if (op == 43 && wc == 4) { /* OpConstant */
            if (s->const_count < 16)
                s->const_values[s->const_count++] = w[pos + 3];
        } else if (op == 44 && wc >= 3) { /* OpConstantComposite */
            uint32_t i;
            s->composite_count = (int)(wc - 3);
            for (i = 0; i < wc - 3 && i < 8; i++)
                s->composite_parts[i] = w[pos + 3 + i];
        } else if (op == 61) {
            s->has_load = 1;
        } else if (op == 62) {
            s->has_store = 1;
        } else if (op == 253) {
            s->has_return = 1;
        } else if (op == 56) {
            s->has_function_end = 1;
        }
        pos += wc;
    }
}

static int has_const(const scan_t *s, uint32_t bits)
{
    int i;
    for (i = 0; i < s->const_count; i++) {
        if (s->const_values[i] == bits)
            return 1;
    }
    return 0;
}

static void write_spv(const char *dir, const char *name,
                      const uint32_t *w, uint32_t count)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL) {
        printf("FAIL: cannot write %s\n", path);
        g_failures++;
        return;
    }
    if (fwrite(w, 4, count, f) != count) {
        printf("FAIL: short write %s\n", path);
        g_failures++;
    }
    fclose(f);
}

static int spirv_val_available(void)
{
    int rc = system("command -v spirv-val >/dev/null 2>&1");
    return rc == 0;
}

static void validate_spv(const char *dir, const char *name)
{
    char cmd[600];
    int rc;
    snprintf(cmd, sizeof(cmd), "spirv-val %s/%s", dir, name);
    rc = system(cmd);
    g_checks++;
    if (rc != 0) {
        g_failures++;
        printf("FAIL: %s failed (exit %d)\n", cmd,
               WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
    }
}

static void test_vertex(const char *dir)
{
    a2s_function_t fn;
    scan_t s;
    int rc;

    memset(&fn, 0, sizeof(fn));
    fn.entry_name = "main";
    fn.stage = A2S_STAGE_VERTEX;
    fn.in_location = 0;
    rc = a2s_emit(&fn, g_words, 512, &g_count);
    CHECK_ERR(rc, A2S_OK);
    if (rc != A2S_OK)
        return;
    CHECK(g_count > 5 && g_count < 512);
    CHECK(g_words[0] == A2S_SPIRV_MAGIC);
    CHECK(g_words[1] == A2S_SPIRV_VERSION);
    CHECK(g_words[3] > 0); /* bound */
    CHECK(g_words[4] == 0); /* schema */
    scan_module(g_words, g_count, &s);
    CHECK(s.entry_model == 0); /* Vertex */
    CHECK(strcmp(s.entry_name, "main") == 0);
    CHECK(s.has_builtin_position == 1);
    CHECK(s.has_location_0 == 1);
    CHECK(s.has_load == 1);
    CHECK(s.has_store == 1);
    CHECK(s.has_return == 1);
    CHECK(s.has_function_end == 1);
    CHECK(s.has_origin_upper_left == 0);
    write_spv(dir, "vertex.spv", g_words, g_count);
}

static void test_fragment(const char *dir)
{
    a2s_function_t fn;
    scan_t s;
    int rc;

    memset(&fn, 0, sizeof(fn));
    fn.entry_name = "main";
    fn.stage = A2S_STAGE_FRAGMENT;
    fn.color[0] = 1.0f;
    fn.color[1] = 0.0f;
    fn.color[2] = 0.0f;
    fn.color[3] = 1.0f;
    rc = a2s_emit(&fn, g_words, 512, &g_count);
    CHECK_ERR(rc, A2S_OK);
    if (rc != A2S_OK)
        return;
    CHECK(g_words[0] == A2S_SPIRV_MAGIC);
    scan_module(g_words, g_count, &s);
    CHECK(s.entry_model == 4); /* Fragment */
    CHECK(strcmp(s.entry_name, "main") == 0);
    CHECK(s.has_origin_upper_left == 1);
    CHECK(s.has_location_0 == 1);
    CHECK(s.has_builtin_position == 0);
    CHECK(s.has_store == 1);
    CHECK(s.has_load == 0);
    CHECK(s.has_return == 1);
    CHECK(s.has_function_end == 1);
    CHECK(s.composite_count == 4);
    CHECK(has_const(&s, 0x3f800000u)); /* 1.0f */
    CHECK(has_const(&s, 0x00000000u)); /* 0.0f */
    write_spv(dir, "fragment.spv", g_words, g_count);
}

static void test_fragment_color(const char *dir)
{
    a2s_function_t fn;
    scan_t s;
    int rc;

    memset(&fn, 0, sizeof(fn));
    fn.entry_name = "main";
    fn.stage = A2S_STAGE_FRAGMENT;
    fn.color[0] = 0.5f;
    fn.color[1] = 0.25f;
    fn.color[2] = 0.75f;
    fn.color[3] = 1.0f;
    rc = a2s_emit(&fn, g_words, 512, &g_count);
    CHECK_ERR(rc, A2S_OK);
    if (rc != A2S_OK)
        return;
    scan_module(g_words, g_count, &s);
    /* Each component gets its own float constant with exact bits. */
    CHECK(has_const(&s, 0x3f000000u)); /* 0.5f */
    CHECK(has_const(&s, 0x3e800000u)); /* 0.25f */
    CHECK(has_const(&s, 0x3f400000u)); /* 0.75f */
    CHECK(has_const(&s, 0x3f800000u)); /* 1.0f */
    CHECK(s.composite_count == 4);
    write_spv(dir, "fragment_color.spv", g_words, g_count);
}

static void test_errors(void)
{
    a2s_function_t fn;
    uint32_t count = 0;

    memset(&fn, 0, sizeof(fn));
    fn.entry_name = "main";
    fn.stage = A2S_STAGE_VERTEX;
    CHECK_ERR(a2s_emit(NULL, g_words, 512, &count), A2S_ERR_BAD_ARG);
    CHECK_ERR(a2s_emit(&fn, NULL, 512, &count), A2S_ERR_BAD_ARG);
    CHECK_ERR(a2s_emit(&fn, g_words, 512, NULL), A2S_ERR_BAD_ARG);
    fn.entry_name = "";
    CHECK_ERR(a2s_emit(&fn, g_words, 512, &count), A2S_ERR_BAD_ARG);
    fn.entry_name = "main";
    fn.stage = A2S_STAGE_KERNEL;
    CHECK_ERR(a2s_emit(&fn, g_words, 512, &count), A2S_ERR_BAD_STAGE);
    fn.stage = A2S_STAGE_VERTEX;
    CHECK_ERR(a2s_emit(&fn, g_words, 5, &count), A2S_ERR_OVERFLOW);
    CHECK_ERR(a2s_emit(&fn, g_words, 0, &count), A2S_ERR_OVERFLOW);
}

int main(int argc, char **argv)
{
    const char *dir = "/tmp/a2s-spv";
    char mkdir_cmd[300];
    int have_val;

    if (argc > 1)
        dir = argv[1];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dir);
    if (system(mkdir_cmd) != 0) {
        printf("FAIL: cannot create %s\n", dir);
        return 1;
    }

    test_vertex(dir);
    test_fragment(dir);
    test_fragment_color(dir);
    test_errors();

    have_val = spirv_val_available();
    if (have_val) {
        validate_spv(dir, "vertex.spv");
        validate_spv(dir, "fragment.spv");
        validate_spv(dir, "fragment_color.spv");
        printf("spirv-val: validated 3 modules\n");
    } else {
        printf("spirv-val: NOT FOUND, validation skipped\n");
    }

    printf("air2spirv_test: %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
