/*
 * timing_plist_test.c - Host unit test for the D47 Default Event Timing Profile.
 *
 * Validates sysroot-overlay/System/Library/EventTimingProfiles/D47.Default.plist:
 * it must be a well-formed XML plist whose top level is a dictionary with
 * zero keys (the minimal profile: UIKit reads its keys, finds none, and uses
 * built-in defaults). The test also proves the parser rejects the old
 * 0-byte workaround file, truncated XML, mismatched tags, trailing junk,
 * and a non-dict top level, and that it counts keys (a one-key dict parses
 * but is rejected by the shipped-file rule).
 *
 * The plist path comes from -DTIMING_PLIST_PATH (set by tools/host_tests.sh),
 * $IOSWINE_TIMING_PLIST, or argv[1], in that order.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* Minimal strict XML-plist reader. Returns 0 on success and fills
 * *out_is_dict (top level is <dict>) and *out_key_count (number of <key>
 * elements directly inside the top-level dict). Returns -1 on any
 * malformed input, including an empty buffer. */
#define PLIST_MAX_DEPTH 32

static int g_stack_depth;
static char g_stack[PLIST_MAX_DEPTH][16];

static int stack_push(const char *name, size_t len) {
    if (g_stack_depth >= PLIST_MAX_DEPTH || len >= 16) return -1;
    memcpy(g_stack[g_stack_depth], name, len);
    g_stack[g_stack_depth][len] = '\0';
    g_stack_depth++;
    return 0;
}

static int stack_top_is(const char *name) {
    if (g_stack_depth == 0) return 0;
    return strcmp(g_stack[g_stack_depth - 1], name) == 0;
}

static void stack_pop(void) {
    if (g_stack_depth > 0) g_stack_depth--;
}

/* Skip one XML comment starting at p (which points at "<!--").
 * Returns pointer just past "-->", or NULL if unterminated. */
static const char *skip_comment(const char *p, const char *end) {
    const char *q = p + 4;
    while (q + 2 < end) {
        if (q[0] == '-' && q[1] == '-' && q[2] == '>') return q + 3;
        q++;
    }
    return NULL;
}

/* Skip whitespace and comments. Returns NULL on unterminated comment. */
static const char *skip_ws_comments(const char *p, const char *end) {
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p + 4 <= end && strncmp(p, "<!--", 4) == 0) {
            p = skip_comment(p, end);
            if (!p) return NULL;
            continue;
        }
        return p;
    }
}

static int parse_plist(const char *buf, size_t len, int *out_is_dict, int *out_key_count) {
    const char *p = buf;
    const char *end = buf + len;
    int saw_xml_decl = 0;
    int saw_plist_open = 0;
    int saw_plist_close = 0;
    int saw_top = 0;
    int is_dict = 0;
    int key_count = 0;
    g_stack_depth = 0;

    if (len == 0) return -1;

    p = skip_ws_comments(p, end);
    if (!p) return -1;

    /* Optional XML declaration <?xml ... ?> */
    if (p + 5 <= end && strncmp(p, "<?xml", 5) == 0) {
        const char *q = p + 5;
        int closed = 0;
        while (q + 1 < end) {
            if (q[0] == '?' && q[1] == '>') { q += 2; closed = 1; break; }
            q++;
        }
        if (!closed) return -1;
        saw_xml_decl = 1;
        p = q;
    }

    for (;;) {
        p = skip_ws_comments(p, end);
        if (!p) return -1;
        if (p >= end) break;
        if (*p != '<') {
            /* Text content inside an element (e.g. <string>x</string>):
             * skip to the next tag. Text outside any element is junk. */
            const char *lt;
            if (g_stack_depth == 0) return -1;
            lt = memchr(p, '<', (size_t)(end - p));
            if (!lt) return -1;
            p = lt;
            continue;
        }

        /* DOCTYPE: <!DOCTYPE ...> (no '>' inside the quoted URL). */
        if (p + 9 <= end && strncmp(p, "<!DOCTYPE", 9) == 0) {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            if (!gt) return -1;
            p = gt + 1;
            continue;
        }
        if (p + 4 <= end && strncmp(p, "<!--", 4) == 0) {
            p = skip_comment(p, end);
            if (!p) return -1;
            continue;
        }
        if (p + 2 <= end && p[1] == '!') return -1; /* other <! markup: reject */
        if (p + 2 <= end && p[1] == '?') return -1; /* stray processing instruction */

        /* Tag: <name>, </name>, or <name/>. */
        {
            int closing = 0;
            const char *name = p + 1;
            const char *q;
            size_t nlen;
            int selfclose = 0;

            if (*name == '/') { closing = 1; name++; }
            q = name;
            while (q < end && *q != '>' && *q != '/' && *q != ' ' && *q != '\t' &&
                   *q != '\n' && *q != '\r') q++;
            nlen = (size_t)(q - name);
            if (nlen == 0 || nlen >= 16) return -1;
            /* Find the closing '>'. */
            while (q < end && *q != '>') q++;
            if (q >= end) return -1;
            if (q > name && q[-1] == '/') selfclose = 1;
            p = q + 1;

            if (closing) {
                char tmp[16];
                memcpy(tmp, name, nlen);
                tmp[nlen] = '\0';
                if (strcmp(tmp, "plist") == 0) {
                    if (g_stack_depth != 0 || !saw_plist_open) return -1;
                    saw_plist_close = 1;
                    continue;
                }
                if (!stack_top_is(tmp)) return -1;
                stack_pop();
                continue;
            }

            /* Opening tag. */
            if (nlen == 5 && strncmp(name, "plist", 5) == 0) {
                if (saw_plist_open || saw_top || g_stack_depth != 0) return -1;
                saw_plist_open = 1;
                continue;
            }
            if (!saw_plist_open) return -1;
            if (saw_plist_close) return -1;
            if (selfclose) {
                /* <true/> <false/> and other empty elements: no stack effect.
                 * A self-closing <dict/> or <array/> counts as a top-level
                 * element but holds no keys. */
                if (!saw_top) {
                    saw_top = 1;
                    is_dict = (nlen == 4 && strncmp(name, "dict", 4) == 0);
                }
                continue;
            }
            if (!saw_top) {
                saw_top = 1;
                is_dict = (nlen == 4 && strncmp(name, "dict", 4) == 0);
                if (stack_push(name, nlen) != 0) return -1;
                continue;
            }
            /* <key> directly inside the top-level dict counts. */
            if (nlen == 3 && strncmp(name, "key", 3) == 0 &&
                g_stack_depth == 1 && stack_top_is("dict")) key_count++;
            if (stack_push(name, nlen) != 0) return -1;
        }
    }

    (void)saw_xml_decl;
    if (!saw_plist_open || !saw_plist_close) return -1;
    if (!saw_top) return -1;
    if (g_stack_depth != 0) return -1;
    *out_is_dict = is_dict;
    *out_key_count = key_count;
    return 0;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    *out_len = (size_t)sz;
    return buf;
}

static void check_buffer(const char *label, const char *buf, size_t len,
                         int want_ok, int want_dict, int want_keys) {
    int is_dict = -1, keys = -1;
    int rc = parse_plist(buf, len, &is_dict, &keys);
    if (!want_ok) {
        CHECK(rc != 0);
    } else {
        CHECK(rc == 0);
        if (rc == 0) {
            CHECK(is_dict == want_dict);
            CHECK(keys == want_keys);
        }
    }
    (void)label;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    char *data = NULL;
    size_t len = 0;
    int is_dict = -1, keys = -1, rc;

#ifdef TIMING_PLIST_PATH
    path = TIMING_PLIST_PATH;
#endif
    {
        const char *env = getenv("IOSWINE_TIMING_PLIST");
        if (env && *env) path = env;
    }
    if (argc > 1) path = argv[1];
    CHECK(path != NULL);
    if (!path) {
        printf("timing_plist_test: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    /* Shipped file: must parse, must be a dict, must have zero keys. */
    data = read_file(path, &len);
    CHECK(data != NULL);
    if (data) {
        CHECK(len > 0);
        CHECK(len < 4096); /* minimal profile: no room for invented keys */
        rc = parse_plist(data, len, &is_dict, &keys);
        CHECK(rc == 0);
        CHECK(is_dict == 1);
        CHECK(keys == 0);
        /* The shipped file carries an XML comment (provenance note); the
         * parser must accept comments anywhere between tags. */
        CHECK(strstr(data, "<!--") != NULL);
        free(data);
    }

    /* Negative cases: the old 0-byte workaround and malformed inputs. */
    check_buffer("empty", "", 0, 0, 0, 0);
    check_buffer("truncated",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>", 48, 0, 0, 0);
    check_buffer("mismatched",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict></array></plist>",
        64, 0, 0, 0);
    check_buffer("trailing-junk",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict></dict></plist>junk",
        67, 0, 0, 0);
    check_buffer("not-a-dict",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><array><string>x</string></array></plist>",
        83, 1, 0, 0);
    check_buffer("unclosed-comment",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><!-- oops <dict></dict></plist>",
        73, 0, 0, 0);

    /* Positive parser cases: comments accepted, keys counted. */
    check_buffer("comment-ok",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><!-- note --><dict></dict></plist>",
        76, 1, 1, 0);
    check_buffer("one-key",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>Foo</key><string>bar</string></dict></plist>",
        97, 1, 1, 1);
    check_buffer("nested-keys-not-counted",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>Outer</key><dict><key>Inner</key><integer>1</integer></dict></dict></plist>",
        128, 1, 1, 1);
    check_buffer("selfclose-dict",
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict/></plist>",
        57, 1, 1, 0);

    /* The one-key dict parses but must FAIL the shipped-file rule
     * (zero keys): prove the rule would catch an invented key. */
    {
        const char *one =
            "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>Foo</key><string>bar</string></dict></plist>";
        rc = parse_plist(one, strlen(one), &is_dict, &keys);
        CHECK(rc == 0);
        CHECK(keys == 1);
        CHECK(keys != 0);
    }

    printf("timing_plist_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
