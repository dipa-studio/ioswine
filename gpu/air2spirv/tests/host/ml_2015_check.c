/* ml_2015_check.c - host check for the 2015 HelloTriangle .metallib
 * rebuilt by tests/host/rebuild_2015.py from the hex dump printed in
 * zhuowei's writeup (https://worthdoingbadly.com/metalbitcode/).
 *
 * Asserts the container facts the research doc derives from that file:
 * 2 functions, vertexShader / fragmentShader, OFFT (0,0,0) and
 * (8,8,0xaf0), MDSZ 0xaf0 / 0x8c0, VERS (2,0,2,0) on both, the header
 * tiling (function list 0x58 size 0x106, public metadata 0x162 size
 * 0x10, private metadata 0x172 size 0x10, bitcode 0x182 size 0x13b0),
 * and the bitcode magic: present exactly at the header's bitcode
 * offset for function 0 (no 8-byte anomaly), absent for function 1
 * because the reconstruction zero-fills the second blob.
 *
 * The HASH tags are NOT checked here: the reconstruction zero-fills
 * the bitcode bodies past 0x19f, so no hash can match. scan.py reports
 * that mismatch; the C reader does not verify hashes at all (that is
 * the scan tool's job).
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -O1 -I../../src \
 *      -o ml_2015_check ml_2015_check.c ../../src/metallib.c
 *   ./ml_2015_check <hello2015.metallib>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "metallib.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static ml_library_t g_lib; /* ~20 KiB; keep off the stack */

int main(int argc, char **argv)
{
    uint8_t *file = NULL;
    size_t len = 0;
    FILE *f;
    int rc;

    if (argc != 2) {
        printf("usage: ml_2015_check <hello2015.metallib>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        printf("cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(len);
    if (file == NULL || fread(file, 1, len, f) != len) {
        printf("cannot read %s\n", argv[1]);
        fclose(f);
        return 2;
    }
    fclose(f);

    CHECK(len == 0x1532);
    rc = ml_open(file, (uint32_t)len, &g_lib);
    CHECK(rc == ML_OK);
    if (rc != ML_OK) {
        printf("ml_open failed: %d (%s)\n", rc, ml_error_string(rc));
        free(file);
        return 1;
    }
    CHECK(g_lib.count == 2);

    /* Header tiling, from the printed dump. */
    CHECK(g_lib.header.platform == ML_PLATFORM_IOS);
    CHECK(g_lib.header.version_major == 2);
    CHECK(g_lib.header.version_minor == 2);
    CHECK(g_lib.header.lib_type == ML_TYPE_EXECUTABLE);
    CHECK(g_lib.header.target_os == ML_OS_UNKNOWN);
    CHECK(g_lib.header.os_major == 0);
    CHECK(g_lib.header.os_minor == 0);
    CHECK(g_lib.header.file_size == 0x1532);
    CHECK(g_lib.header.func_list_off == 0x58);
    CHECK(g_lib.header.func_list_size == 0x106);
    CHECK(g_lib.header.pub_meta_off == 0x162);
    CHECK(g_lib.header.pub_meta_size == 0x10);
    CHECK(g_lib.header.priv_meta_off == 0x172);
    CHECK(g_lib.header.priv_meta_size == 0x10);
    CHECK(g_lib.header.bitcode_off == 0x182);
    CHECK(g_lib.header.bitcode_size == 0x13b0);

    /* Function 0: vertexShader. */
    CHECK(strcmp(g_lib.funcs[0].name, "vertexShader") == 0);
    CHECK(g_lib.funcs[0].func_type == ML_FUNC_VERTEX);
    CHECK(g_lib.funcs[0].offt[0] == 0);
    CHECK(g_lib.funcs[0].offt[1] == 0);
    CHECK(g_lib.funcs[0].offt[2] == 0);
    CHECK(g_lib.funcs[0].bitcode_size == 0xaf0);
    CHECK(g_lib.funcs[0].bitcode_off == 0x182);
    CHECK(g_lib.funcs[0].air_major == 2);
    CHECK(g_lib.funcs[0].air_minor == 0);
    CHECK(g_lib.funcs[0].lang_major == 2);
    CHECK(g_lib.funcs[0].lang_minor == 0);
    CHECK(g_lib.funcs[0].has_hash == 1);
    CHECK(g_lib.funcs[0].hash[0] == 0x6d);
    CHECK(g_lib.funcs[0].hash[31] == 0xff);
    /* Wrapper magic exactly at the header's bitcode offset: no
     * 8-byte anomaly. */
    CHECK(g_lib.funcs[0].bitcode_ok == 1);

    /* Function 1: fragmentShader. */
    CHECK(strcmp(g_lib.funcs[1].name, "fragmentShader") == 0);
    CHECK(g_lib.funcs[1].func_type == ML_FUNC_FRAGMENT);
    CHECK(g_lib.funcs[1].offt[0] == 8);
    CHECK(g_lib.funcs[1].offt[1] == 8);
    CHECK(g_lib.funcs[1].offt[2] == 0xaf0);
    CHECK(g_lib.funcs[1].bitcode_size == 0x8c0);
    CHECK(g_lib.funcs[1].bitcode_off == 0xc72);
    CHECK(g_lib.funcs[1].air_major == 2);
    CHECK(g_lib.funcs[1].air_minor == 0);
    CHECK(g_lib.funcs[1].lang_major == 2);
    CHECK(g_lib.funcs[1].lang_minor == 0);
    CHECK(g_lib.funcs[1].has_hash == 1);
    CHECK(g_lib.funcs[1].hash[0] == 0x21);
    CHECK(g_lib.funcs[1].hash[31] == 0x8c);
    /* Second blob is zero-filled in the reconstruction, so its
     * wrapper magic is absent. That is expected, not a parse failure. */
    CHECK(g_lib.funcs[1].bitcode_ok == 0);

    printf("ml_2015_check: %d checks, %d failures\n",
           g_checks, g_failures);
    free(file);
    return g_failures == 0 ? 0 : 1;
}
