/*
 * mg.c: MobileGestalt service for ioswine.
 *
 * Owns the two answer mechanisms: the on-disk cache file (primary)
 * and the MobileGestaltHelper XPC service (fallback). The static
 * answer table below is the single source of truth for both:
 * mg_build_cache serialises it into the bplist00 cache file,
 * mg_svchost_handler answers XPC questions from it.
 *
 * Target device for every value: iPhone17,3 (iPhone 16), iOS 26.6.2,
 * build 23G90. Main-screen answers match the BKS fake (1440x3200 @
 * 4.0), not the iPhone panel. Protected questions are denied, never
 * fabricated. CacheData is a zero-filled opaque blob. See mg_names.h
 * for the [src]/[guess] tags on every value.
 *
 * Freestanding C. No function-pointer tables in static data (the
 * table is data, dispatch is if/switch). Local string helpers follow
 * the tree's my, sh and bp helper pattern (no libc).
 *
 * No em dashes are used in this file.
 */
#include "mg.h"
#include "mg_names.h"
#include "bplist.h"
#include "bsxpc.h"
#include "container.h"
#include "nsxpc.h"

/* ------------------------------------------------------------------ */
/* Local helpers (freestanding).                                       */
/* ------------------------------------------------------------------ */

static size_t mg_strlen(const char *s) {
    size_t n = 0;
    if (s == NULL) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static int mg_strcmp(const char *a, const char *b) {
    if (a == NULL || b == NULL) return (a == b) ? 0 : (a == NULL ? -1 : 1);
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void mg_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

/* Append "/" + rel to the NUL-terminated path in out (cap bytes),
 * adding the slash only when out does not already end in one.
 * Returns 0 or XW_ERR_NOSPACE. */
static int mg_path_append(char *out, size_t cap, const char *rel) {
    size_t n = mg_strlen(out);
    size_t m = mg_strlen(rel);
    size_t slash = (n > 0 && out[n - 1] != '/') ? 1 : 0;
    if (n + slash + m + 1 > cap) return XW_ERR_NOSPACE;
    if (slash != 0) {
        out[n] = '/';
        n++;
    }
    mg_memcpy(out + n, rel, m + 1);
    return 0;
}

/* mkdir -p for the directory part of path: create every directory
 * level up to (not including) the last component, ignoring errors
 * (EEXIST when a level already exists). The cache write must not
 * depend on prior directory creation: on a fresh temp home (host
 * test) or a fresh fake filesystem, no level above the file may
 * exist yet. */
static void mg_mkdir_p(const char *path) {
    char tmp[MG_CACHE_PATH_CAP];
    size_t n = mg_strlen(path);
    size_t i;
    if (n == 0 || n >= sizeof(tmp)) return;
    mg_memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            (void)lsk_mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
}

/* ------------------------------------------------------------------ */
/* Static answer data.                                                 */
/* ------------------------------------------------------------------ */

/* MainScreenStaticInfo, 24 bytes: 6 x little-endian u32/f32
 * (width px, height px, pitch ppi, scale float, orientation float,
 * class u32) for 1440x3200 @ 521 ppi @ 4.0, class 8. Base64 of these
 * bytes is oAUAAIAMAAAJAgAAAACAQAAAAAAIAAAA, matching the doc's
 * computed value for the host geometry. Layout is [guess] (one
 * published sample), the bytes are pinned by that base64 string.
 * Pitch is [guess]: 1440x3200 on the 6.73 inch Xiaomi 12 Pro panel
 * is about 521 ppi (sqrt(1440^2+3200^2)/6.73). */
static const uint8_t mg_static_info[24] = {
    0xA0, 0x05, 0x00, 0x00, 0x80, 0x0C, 0x00, 0x00,
    0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0x80, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
};

/* MainScreenCanvasSizes: one CGSize {1440.0, 3200.0} as 2 x float64
 * little-endian (16 bytes). Pixels, not points [guess]. */
static const uint8_t mg_canvas_sizes[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x96, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA9, 0x40
};

/* All-zero 52-byte opaque field (MG_K_ZERO_52) [src: DeepWiki]. */
static const uint8_t mg_zero_52[52] = { 0 };

/* CacheData blob: zero-filled, MG_CACHEDATA_LEN bytes (BSS). */
static uint8_t mg_cache_data[MG_CACHEDATA_LEN];

static const char * const mg_compatible_variants[] = {
    MG_V_COMPATIBLE_APP_VARIANT_0, MG_V_COMPATIBLE_APP_VARIANT_1
};

static const int64_t mg_device_families[] = { 1 };

typedef struct {
    const char *question;
    const char *key; /* obfuscated CacheExtra key */
    mg_answer_t ans;
} mg_entry_t;

/* The launch-time answer table. Values are iPhone17,3 / 26.6.2. */
static const mg_entry_t mg_table[] = {
    { MG_Q_DEVICE_CLASS, MG_K_DEVICE_CLASS,
      { MG_ANS_STRING, MG_V_DEVICE_CLASS, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_DEVICE_CLASS_NUMBER, MG_K_DEVICE_CLASS_NUMBER,
      { MG_ANS_INT, NULL, 1, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_DEVICE_NAME, MG_K_DEVICE_NAME,
      { MG_ANS_STRING, MG_V_DEVICE_NAME, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_GS_DEVICE_NAME, MG_K_GS_DEVICE_NAME,
      { MG_ANS_STRING, MG_V_GS_DEVICE_NAME, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_PRODUCT_TYPE, MG_K_PRODUCT_TYPE,
      { MG_ANS_STRING, MG_V_PRODUCT_TYPE, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_PRODUCT_VERSION, MG_K_PRODUCT_VERSION,
      { MG_ANS_STRING, MG_V_PRODUCT_VERSION, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_BUILD_VERSION, MG_K_BUILD_VERSION,
      { MG_ANS_STRING, MG_V_BUILD_VERSION, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_PRODUCT_NAME, MG_K_PRODUCT_NAME,
      { MG_ANS_STRING, MG_V_PRODUCT_NAME, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_HW_MODEL_STR, MG_K_HW_MODEL_STR,
      { MG_ANS_STRING, MG_V_HW_MODEL_STR, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_IS_SIMULATOR, MG_K_IS_SIMULATOR,
      { MG_ANS_BOOL, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_INTERNAL_BUILD, MG_K_INTERNAL_BUILD,
      { MG_ANS_BOOL, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_APPLE_INTERNAL_INSTALL, MG_K_APPLE_INTERNAL_INSTALL,
      { MG_ANS_BOOL, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_CPU_ARCHITECTURE, MG_K_CPU_ARCHITECTURE,
      { MG_ANS_STRING, MG_V_CPU_ARCHITECTURE, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_SUPPORTED_DEVICE_FAMILIES, MG_K_SUPPORTED_DEVICE_FAMILIES,
      { MG_ANS_INT_ARRAY, NULL, 0, 0.0, 0, NULL, 0, NULL, mg_device_families, 1 } },
    { MG_Q_REGION_CODE, MG_K_REGION_CODE,
      { MG_ANS_STRING, MG_V_REGION_CODE, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_REGION_INFO, MG_K_REGION_INFO,
      { MG_ANS_STRING, MG_V_REGION_INFO, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_REGULATORY_MODEL_NUMBER, MG_K_REGULATORY_MODEL_NUMBER,
      { MG_ANS_STRING, MG_V_REGULATORY_MODEL_NUMBER, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MODEL_NUMBER, MG_K_MODEL_NUMBER,
      { MG_ANS_STRING, MG_V_MODEL_NUMBER, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MARKETING_NAME, MG_K_MARKETING_NAME,
      { MG_ANS_STRING, MG_V_MARKETING_NAME, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_DEVICE_COLOR_MAP_POLICY, MG_K_DEVICE_COLOR_MAP_POLICY,
      { MG_ANS_INT, NULL, 1, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_HAS_BASEBAND, MG_K_HAS_BASEBAND,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_CONTAINS_CELLULAR_RADIO, MG_K_CONTAINS_CELLULAR_RADIO,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_CELLULAR_TELEPHONY, MG_K_CELLULAR_TELEPHONY,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_IPAD, MG_K_IPAD,
      { MG_ANS_BOOL, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_ARM64, MG_K_ARM64,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_DEVICE_SUPPORTS_64BIT, MG_K_DEVICE_SUPPORTS_64BIT,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_DEVICE_SUPPORTS_ARKIT, MG_K_DEVICE_SUPPORTS_ARKIT,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_METAL_CAPABILITY, MG_K_METAL_CAPABILITY,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_HAS_EXTENDED_COLOR_DISPLAY, MG_K_HAS_EXTENDED_COLOR_DISPLAY,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_DEVICE_CORNER_RADIUS, MG_K_DEVICE_CORNER_RADIUS,
      { MG_ANS_REAL, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_SUPPORTS_FORCE_TOUCH, MG_K_SUPPORTS_FORCE_TOUCH,
      { MG_ANS_BOOL, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MAIN_DISPLAY_ROTATION, MG_K_MAIN_DISPLAY_ROTATION,
      { MG_ANS_INT, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_HAS_SPRINGBOARD, MG_K_HAS_SPRINGBOARD,
      { MG_ANS_BOOL, NULL, 0, 0.0, 1, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_COMPATIBLE_APP_VARIANTS, MG_K_COMPATIBLE_APP_VARIANTS,
      { MG_ANS_STRING_ARRAY, NULL, 0, 0.0, 0, NULL, 0, mg_compatible_variants, NULL, 2 } },
    { MG_Q_DEVICE_ENCLOSURE_COLOR, MG_K_DEVICE_ENCLOSURE_COLOR,
      { MG_ANS_STRING, MG_V_DEVICE_ENCLOSURE_COLOR, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_DEVICE_COLOR, MG_K_DEVICE_COLOR,
      { MG_ANS_STRING, MG_V_DEVICE_COLOR, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MAIN_SCREEN_WIDTH, MG_K_MAIN_SCREEN_WIDTH,
      { MG_ANS_INT, NULL, 1440, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MAIN_SCREEN_HEIGHT, MG_K_MAIN_SCREEN_HEIGHT,
      { MG_ANS_INT, NULL, 3200, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MAIN_SCREEN_SCALE, MG_K_MAIN_SCREEN_SCALE,
      { MG_ANS_REAL, NULL, 0, 4.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MAIN_SCREEN_PITCH, MG_K_MAIN_SCREEN_PITCH,
      { MG_ANS_INT, NULL, 521, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MAIN_SCREEN_CLASS, MG_K_MAIN_SCREEN_CLASS,
      { MG_ANS_INT, NULL, 8, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MAIN_SCREEN_STATIC_INFO, MG_K_MAIN_SCREEN_STATIC_INFO,
      { MG_ANS_DATA, NULL, 0, 0.0, 0, mg_static_info, 24, NULL, NULL, 0 } },
    { MG_Q_MAIN_SCREEN_ORIENTATION, MG_K_MAIN_SCREEN_ORIENTATION,
      { MG_ANS_REAL, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
    { MG_Q_MAIN_SCREEN_CANVAS_SIZES, MG_K_MAIN_SCREEN_CANVAS_SIZES,
      { MG_ANS_DATA, NULL, 0, 0.0, 0, mg_canvas_sizes, 16, NULL, NULL, 0 } },
    { MG_Q_ARTWORK_TRAITS, MG_K_ARTWORK_TRAITS,
      { MG_ANS_ARTWORK_DICT, NULL, 0, 0.0, 0, NULL, 0, NULL, NULL, 0 } },
};

#define MG_TABLE_COUNT (sizeof(mg_table) / sizeof(mg_table[0]))

/* Cache-only duplicate keys: same value under several obfuscated
 * keys. Only the canonical key of
 * each value is in mg_table; these extras are filled in the cache
 * file only. */
typedef struct {
    const char *key;
    const char *value;
} mg_dup_t;

static const mg_dup_t mg_dups[] = {
    { MG_K_DUP_PRODUCT_TYPE_0, MG_V_PRODUCT_TYPE },
    { MG_K_DUP_PRODUCT_TYPE_1, MG_V_PRODUCT_TYPE },
    { MG_K_DUP_PRODUCT_TYPE_2, MG_V_PRODUCT_TYPE },
    { MG_K_DUP_PRODUCT_TYPE_3, MG_V_PRODUCT_TYPE },
    { MG_K_DUP_PRODUCT_TYPE_4, MG_V_PRODUCT_TYPE },
    { MG_K_DUP_PRODUCT_TYPE_5, MG_V_PRODUCT_TYPE },
    { MG_K_DUP_PRODUCT_TYPE_6, MG_V_PRODUCT_TYPE },
    { MG_K_DUP_PRODUCT_TYPE_7, MG_V_PRODUCT_TYPE },
    { MG_K_DUP_DEVICE_CLASS_0, MG_V_DEVICE_CLASS },
    { MG_K_DUP_DEVICE_CLASS_1, MG_V_DEVICE_CLASS },
    { MG_K_DUP_HW_MODEL_STR_0, MG_V_HW_MODEL_STR },
    { MG_K_DUP_HW_MODEL_STR_1, MG_V_HW_MODEL_STR },
    { MG_K_DUP_HW_MODEL_STR_2, MG_V_HW_MODEL_STR },
    { MG_K_DUP_HW_MODEL_STR_3, MG_V_HW_MODEL_STR },
    { MG_K_DUP_HW_MODEL_STR_4, MG_V_HW_MODEL_STR },
    { MG_K_DUP_HW_MODEL_STR_5, MG_V_HW_MODEL_STR },
    { MG_K_DUP_HW_MODEL_STR_6, MG_V_HW_MODEL_STR },
    { MG_K_DUP_HW_MODEL_STR_7, MG_V_HW_MODEL_STR },
    { MG_K_DUP_MARKETING_NAME_0, MG_V_MARKETING_NAME },
};

#define MG_DUP_COUNT (sizeof(mg_dups) / sizeof(mg_dups[0]))

/* Protected questions: denied, never fabricated. */
static const char * const mg_protected[] = {
    MG_Q_UNIQUE_DEVICE_ID,
    MG_Q_UNIQUE_DEVICE_ID_DATA,
    MG_Q_SERIAL_NUMBER,
    MG_Q_MLB_SERIAL_NUMBER,
    MG_Q_BLUETOOTH_ADDRESS,
    MG_Q_INVERSE_DEVICE_ID,
    MG_Q_UNIQUE_CHIP_ID,
    MG_Q_MOBILE_EQUIPMENT_ID,
};

#define MG_PROTECTED_COUNT (sizeof(mg_protected) / sizeof(mg_protected[0]))

/* Dynamic questions: no static answer, the helper returns NULL. */
static const char * const mg_dynamic[] = {
    MG_Q_BATTERY_CURRENT_CAPACITY,
    MG_Q_BATTERY_IS_CHARGING,
    MG_Q_AIRPLANE_MODE,
    MG_Q_SIM_STATUS,
    MG_Q_ACTIVE_WIRELESS_TECHNOLOGY,
};

#define MG_DYNAMIC_COUNT (sizeof(mg_dynamic) / sizeof(mg_dynamic[0]))

static int g_mg_inited = 0;

/* ------------------------------------------------------------------ */
/* Lookups.                                                            */
/* ------------------------------------------------------------------ */

int mg_answer_for(const char *question, mg_answer_t *out) {
    uint32_t i;
    if (question == NULL) return XW_ERR_NOT_FOUND;
    for (i = 0; i < MG_TABLE_COUNT; i++) {
        if (mg_strcmp(question, mg_table[i].question) == 0) {
            if (out != NULL) {
                /* Field-by-field copy: a struct assignment here
                 * lowers to a memcpy call, and the freestanding
                 * tree provides no libc memcpy (every file rolls
                 * its own static helper). */
                out->kind = mg_table[i].ans.kind;
                out->s = mg_table[i].ans.s;
                out->i = mg_table[i].ans.i;
                out->d = mg_table[i].ans.d;
                out->b = mg_table[i].ans.b;
                out->data = mg_table[i].ans.data;
                out->data_len = mg_table[i].ans.data_len;
                out->strs = mg_table[i].ans.strs;
                out->ints = mg_table[i].ans.ints;
                out->count = mg_table[i].ans.count;
            }
            return 0;
        }
    }
    return XW_ERR_NOT_FOUND;
}

int mg_is_protected(const char *question) {
    uint32_t i;
    if (question == NULL) return 0;
    for (i = 0; i < MG_PROTECTED_COUNT; i++) {
        if (mg_strcmp(question, mg_protected[i]) == 0) return 1;
    }
    return 0;
}

int mg_is_dynamic(const char *question) {
    uint32_t i;
    if (question == NULL) return 0;
    for (i = 0; i < MG_DYNAMIC_COUNT; i++) {
        if (mg_strcmp(question, mg_dynamic[i]) == 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cache file.                                                         */
/* ------------------------------------------------------------------ */

int mg_cache_path(char *out, size_t cap) {
    int rc;
    if (out == NULL || cap == 0) return XW_ERR_NOSPACE;
    rc = container_group(MG_CACHE_GROUP_ID, out, cap);
    if (rc != 0) return rc;
    return mg_path_append(out, cap, MG_CACHE_REL_PATH);
}

/* bplist setters for the nested dicts. */

static int mg_bp_set_string(bp_arena *a, bp_value *d, const char *key,
                            const char *val) {
    bp_value *v = bp_new_string(a, val);
    if (v == NULL) return XW_ERR_NOSPACE;
    return bp_dict_set(a, d, key, v);
}

static int mg_bp_set_int(bp_arena *a, bp_value *d, const char *key,
                         int64_t val) {
    bp_value *v = bp_new_int(a, val);
    if (v == NULL) return XW_ERR_NOSPACE;
    return bp_dict_set(a, d, key, v);
}

static int mg_bp_set_bool(bp_arena *a, bp_value *d, const char *key,
                          int val) {
    bp_value *v = bp_new_bool(a, val);
    if (v == NULL) return XW_ERR_NOSPACE;
    return bp_dict_set(a, d, key, v);
}

/* The ArtworkTraits nested dict (CacheExtra under MG_K_ARTWORK_TRAITS). */
static bp_value *mg_artwork_bp(bp_arena *a) {
    bp_value *d = bp_new(a, BP_DICT);
    int r = 0;
    if (d == NULL) return NULL;
    if (r == 0) r = mg_bp_set_string(a, d, MG_ART_DEVICE_IDIOM,
                                    MG_V_ART_DEVICE_IDIOM);
    if (r == 0) r = mg_bp_set_int(a, d, MG_ART_DEVICE_SUBTYPE,
                                 MG_V_ART_DEVICE_SUBTYPE);
    if (r == 0) r = mg_bp_set_int(a, d, MG_ART_DEVICE_SCALE_FACTOR,
                                 MG_V_ART_DEVICE_SCALE_FACTOR);
    if (r == 0) r = mg_bp_set_string(a, d, MG_ART_DISPLAY_GAMUT,
                                    MG_V_ART_DISPLAY_GAMUT);
    if (r == 0) r = mg_bp_set_string(a, d, MG_ART_DYNAMIC_DISPLAY_MODE,
                                    MG_V_ART_DYNAMIC_DISPLAY_MODE);
    if (r == 0) r = mg_bp_set_string(a, d, MG_ART_PRODUCT_DESCRIPTION,
                                    MG_V_ART_PRODUCT_DESCRIPTION);
    if (r == 0) r = mg_bp_set_string(a, d, MG_ART_GRAPHICS_FEATURE_SET_CLASS,
                                    MG_V_ART_GRAPHICS_FEATURE_SET_CLASS);
    if (r == 0) r = mg_bp_set_string(a, d, MG_ART_GRAPHICS_FEATURE_SET_FALLBACKS,
                                    MG_V_ART_GRAPHICS_FEATURE_SET_FALLBACKS);
    if (r == 0) r = mg_bp_set_string(a, d, MG_ART_COMPATIBLE_DEVICE_FALLBACK,
                                    MG_V_ART_COMPATIBLE_DEVICE_FALLBACK);
    if (r != 0) return NULL;
    return d;
}

/* The GPU buffer-compression nested dict (CacheExtra under
 * MG_K_GPU_COMPRESSION). Values are the iPhone13,4 values [src:
 * DeepWiki]; the A18 values are [guess]. */
static bp_value *mg_gpu_bp(bp_arena *a) {
    bp_value *d = bp_new(a, BP_DICT);
    int r = 0;
    if (d == NULL) return NULL;
    if (r == 0) r = mg_bp_set_bool(a, d, MG_GPU_BUFFER_COMPRESSION, 0);
    if (r == 0) r = mg_bp_set_bool(a, d, MG_GPU_MEDIA_COMPRESSION, 0);
    if (r == 0) r = mg_bp_set_bool(a, d, MG_GPU_UNIVERSAL_BUFFER_COMPRESSION, 1);
    if (r == 0) r = mg_bp_set_bool(a, d,
        MG_GPU_UNIVERSAL_BUFFER_COMPRESSION_ALIGNMENT_2048, 0);
    if (r == 0) r = mg_bp_set_bool(a, d,
        MG_GPU_UNIVERSAL_LOSSY_BUFFER_COMPRESSION, 0);
    if (r == 0) r = mg_bp_set_bool(a, d,
        MG_GPU_UNIVERSAL_MULTISLICE_BUFFER_COMPRESSION, 0);
    if (r == 0) r = mg_bp_set_bool(a, d,
        MG_GPU_UNIVERSAL_PARGB10101010_BUFFER_COMPRESSION, 0);
    if (r != 0) return NULL;
    return d;
}

/* Add one table entry to a bp dict under its obfuscated key. */
static int mg_add_entry(bp_arena *a, bp_value *dict,
                        const mg_entry_t *e) {
    bp_value *v = NULL;
    uint32_t j;
    switch (e->ans.kind) {
    case MG_ANS_STRING:
        v = bp_new_string(a, e->ans.s);
        break;
    case MG_ANS_INT:
        v = bp_new_int(a, e->ans.i);
        break;
    case MG_ANS_BOOL:
        v = bp_new_bool(a, e->ans.b);
        break;
    case MG_ANS_REAL:
        v = bp_new_real(a, e->ans.d);
        break;
    case MG_ANS_DATA:
        v = bp_new_data(a, e->ans.data, e->ans.data_len);
        break;
    case MG_ANS_STRING_ARRAY:
        v = bp_new(a, BP_ARRAY);
        if (v == NULL) return XW_ERR_NOSPACE;
        for (j = 0; j < e->ans.count; j++) {
            bp_value *el = bp_new_string(a, e->ans.strs[j]);
            if (el == NULL) return XW_ERR_NOSPACE;
            if (bp_array_append(a, v, el) != 0) return XW_ERR_NOSPACE;
        }
        break;
    case MG_ANS_INT_ARRAY:
        v = bp_new(a, BP_ARRAY);
        if (v == NULL) return XW_ERR_NOSPACE;
        for (j = 0; j < e->ans.count; j++) {
            bp_value *el = bp_new_int(a, e->ans.ints[j]);
            if (el == NULL) return XW_ERR_NOSPACE;
            if (bp_array_append(a, v, el) != 0) return XW_ERR_NOSPACE;
        }
        break;
    case MG_ANS_ARTWORK_DICT:
        v = mg_artwork_bp(a);
        break;
    default:
        return XW_ERR_BAD_TYPE;
    }
    if (v == NULL) return XW_ERR_NOSPACE;
    return bp_dict_set(a, dict, e->key, v);
}

int mg_build_cache(uint8_t *out, uint32_t cap, uint32_t *out_len) {
    static uint8_t arena_buf[MG_CACHE_ARENA_CAP];
    bp_arena arena;
    bp_value *root = NULL;
    bp_value *extra = NULL;
    bp_value *v = NULL;
    uint32_t i;
    int r;
    if (out == NULL || out_len == NULL) return XW_ERR_NOSPACE;
    bp_arena_init(&arena, arena_buf, sizeof(arena_buf));
    root = bp_new(&arena, BP_DICT);
    if (root == NULL) return XW_ERR_NOSPACE;
    extra = bp_new(&arena, BP_DICT);
    if (extra == NULL) return XW_ERR_NOSPACE;
    /* CacheData: zero-filled opaque blob. */
    v = bp_new_data(&arena, mg_cache_data, MG_CACHEDATA_LEN);
    if (v == NULL) return XW_ERR_NOSPACE;
    r = bp_dict_set(&arena, root, MG_KEY_CACHE_DATA, v);
    if (r != 0) return r;
    /* CacheExtra: the answer table, then the duplicate keys, then
     * the opaque zero field and the GPU sub-dict. */
    for (i = 0; i < MG_TABLE_COUNT; i++) {
        r = mg_add_entry(&arena, extra, &mg_table[i]);
        if (r != 0) return r;
    }
    for (i = 0; i < MG_DUP_COUNT; i++) {
        r = mg_bp_set_string(&arena, extra, mg_dups[i].key,
                             mg_dups[i].value);
        if (r != 0) return r;
    }
    v = bp_new_data(&arena, mg_zero_52, sizeof(mg_zero_52));
    if (v == NULL) return XW_ERR_NOSPACE;
    r = bp_dict_set(&arena, extra, MG_K_ZERO_52, v);
    if (r != 0) return r;
    v = mg_gpu_bp(&arena);
    if (v == NULL) return XW_ERR_NOSPACE;
    r = bp_dict_set(&arena, extra, MG_K_GPU_COMPRESSION, v);
    if (r != 0) return r;
    r = bp_dict_set(&arena, root, MG_KEY_CACHE_EXTRA, extra);
    if (r != 0) return r;
    /* CacheUUID and CacheVersion. */
    r = mg_bp_set_string(&arena, root, MG_KEY_CACHE_UUID, MG_CACHE_UUID);
    if (r != 0) return r;
    r = mg_bp_set_string(&arena, root, MG_KEY_CACHE_VERSION,
                         MG_CACHE_VERSION);
    if (r != 0) return r;
    return bp_write(root, out, cap, out_len);
}

int mg_write_cache(void) {
    static uint8_t out_buf[MG_CACHE_BUILD_CAP];
    char path[MG_CACHE_PATH_CAP];
    uint32_t out_len = 0;
    uint32_t off = 0;
    int fd;
    int rc;
    rc = mg_build_cache(out_buf, sizeof(out_buf), &out_len);
    if (rc != 0) return rc;
    rc = mg_cache_path(path, sizeof(path));
    if (rc != 0) return rc;
    /* Create the parent directories when needed (mkdir -p); the
     * open below is the real test. */
    mg_mkdir_p(path);
    fd = lsk_open(path, MG_O_WRONLY | MG_O_CREAT | MG_O_TRUNC, 0644);
    if (fd < 0) return XW_ERR_NOT_FOUND;
    while (off < out_len) {
        int64_t w = lsk_write(fd, out_buf + off, out_len - off);
        if (w <= 0) {
            (void)lsk_close(fd);
            return XW_ERR_NOSPACE;
        }
        off += (uint32_t)w;
    }
    if (lsk_close(fd) != 0) return XW_ERR_NOT_FOUND;
    return 0;
}

int mg_cache_valid(void) {
    static uint8_t read_buf[MG_CACHE_READ_CAP];
    static uint8_t arena_buf[MG_CACHE_ARENA_CAP];
    char path[MG_CACHE_PATH_CAP];
    bp_arena arena;
    bp_value *root = NULL;
    bp_value *extra = NULL;
    bp_value *v = NULL;
    int fd;
    int64_t n;
    uint32_t total = 0;
    if (mg_cache_path(path, sizeof(path)) != 0) return 0;
    fd = lsk_open(path, MG_O_RDONLY);
    if (fd < 0) return 0;
    for (;;) {
        if (total >= sizeof(read_buf)) {
            (void)lsk_close(fd);
            return 0;
        }
        n = lsk_read(fd, read_buf + total, sizeof(read_buf) - total);
        if (n < 0) {
            (void)lsk_close(fd);
            return 0;
        }
        if (n == 0) break;
        total += (uint32_t)n;
    }
    (void)lsk_close(fd);
    if (total == 0) return 0;
    bp_arena_init(&arena, arena_buf, sizeof(arena_buf));
    if (bp_parse(read_buf, total, &arena, &root) != 0) return 0;
    if (root == NULL || root->type != BP_DICT) return 0;
    v = bp_dict_get(root, MG_KEY_CACHE_VERSION);
    if (v == NULL || v->type != BP_STRING || v->s == NULL) return 0;
    if (mg_strcmp((const char *)v->s, MG_CACHE_VERSION) != 0) return 0;
    if (bp_dict_get(root, MG_KEY_CACHE_DATA) == NULL) return 0;
    if (bp_dict_get(root, MG_KEY_CACHE_UUID) == NULL) return 0;
    extra = bp_dict_get(root, MG_KEY_CACHE_EXTRA);
    if (extra == NULL || extra->type != BP_DICT) return 0;
    v = bp_dict_get(extra, MG_K_PRODUCT_TYPE);
    if (v == NULL || v->type != BP_STRING || v->s == NULL) return 0;
    if (mg_strcmp((const char *)v->s, MG_V_PRODUCT_TYPE) != 0) return 0;
    return 1;
}

int mg_ensure_cache(void) {
    if (mg_cache_valid()) return 0;
    return mg_write_cache();
}

/* ------------------------------------------------------------------ */
/* XPC service.                                                        */
/* ------------------------------------------------------------------ */

/* The ArtworkTraits nested dict in an XPC reply. */
static int mg_artwork_xw(xw_builder *b, const char *key) {
    int r = xw_dict_add_dict_begin(b, key);
    if (r == XW_OK) r = xw_dict_add_string(b, MG_ART_DEVICE_IDIOM,
                                          MG_V_ART_DEVICE_IDIOM);
    if (r == XW_OK) r = xw_dict_add_int64(b, MG_ART_DEVICE_SUBTYPE,
                                         MG_V_ART_DEVICE_SUBTYPE);
    if (r == XW_OK) r = xw_dict_add_int64(b, MG_ART_DEVICE_SCALE_FACTOR,
                                         MG_V_ART_DEVICE_SCALE_FACTOR);
    if (r == XW_OK) r = xw_dict_add_string(b, MG_ART_DISPLAY_GAMUT,
                                          MG_V_ART_DISPLAY_GAMUT);
    if (r == XW_OK) r = xw_dict_add_string(b, MG_ART_DYNAMIC_DISPLAY_MODE,
                                          MG_V_ART_DYNAMIC_DISPLAY_MODE);
    if (r == XW_OK) r = xw_dict_add_string(b, MG_ART_PRODUCT_DESCRIPTION,
                                          MG_V_ART_PRODUCT_DESCRIPTION);
    if (r == XW_OK) r = xw_dict_add_string(b,
        MG_ART_GRAPHICS_FEATURE_SET_CLASS,
        MG_V_ART_GRAPHICS_FEATURE_SET_CLASS);
    if (r == XW_OK) r = xw_dict_add_string(b,
        MG_ART_GRAPHICS_FEATURE_SET_FALLBACKS,
        MG_V_ART_GRAPHICS_FEATURE_SET_FALLBACKS);
    if (r == XW_OK) r = xw_dict_add_string(b,
        MG_ART_COMPATIBLE_DEVICE_FALLBACK,
        MG_V_ART_COMPATIBLE_DEVICE_FALLBACK);
    if (r == XW_OK) r = xw_dict_end(b);
    return r;
}

/* Add one answer value to an XPC reply dict under key. */
static int mg_add_answer_xw(xw_builder *b, const char *key,
                            const mg_answer_t *ans) {
    int r = XW_OK;
    uint32_t j;
    switch (ans->kind) {
    case MG_ANS_STRING:
        r = xw_dict_add_string(b, key, ans->s);
        break;
    case MG_ANS_INT:
        r = xw_dict_add_int64(b, key, ans->i);
        break;
    case MG_ANS_BOOL:
        r = xw_dict_add_bool(b, key, ans->b);
        break;
    case MG_ANS_REAL:
        r = xw_dict_add_double(b, key, ans->d);
        break;
    case MG_ANS_DATA:
        r = xw_dict_add_data(b, key, ans->data, ans->data_len);
        break;
    case MG_ANS_STRING_ARRAY:
        r = xw_dict_add_array_begin(b, key);
        for (j = 0; r == XW_OK && j < ans->count; j++)
            r = xw_add_string(b, ans->strs[j]);
        if (r == XW_OK) r = xw_array_end(b);
        break;
    case MG_ANS_INT_ARRAY:
        r = xw_dict_add_array_begin(b, key);
        for (j = 0; r == XW_OK && j < ans->count; j++)
            r = xw_add_int64(b, ans->ints[j]);
        if (r == XW_OK) r = xw_array_end(b);
        break;
    case MG_ANS_ARTWORK_DICT:
        r = mg_artwork_xw(b, key);
        break;
    default:
        r = XW_ERR_BAD_TYPE;
        break;
    }
    return r;
}

/* Build {"answer": null, "error": code} in the reply builder. */
static int mg_reply_null(xw_builder *reply, int64_t err_code) {
    int r = xw_dict_begin(reply);
    if (r == XW_OK) r = xw_dict_add_null(reply, MG_REPLY_KEY_ANSWER);
    if (r == XW_OK) r = xw_dict_add_int64(reply, MG_REPLY_KEY_ERROR,
                                         err_code);
    if (r == XW_OK) r = xw_dict_end(reply);
    return r;
}

int mg_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx) {
    xw_value root;
    const char *q = NULL;
    mg_answer_t ans;
    int r;
    (void)ctx;
    if (bsxpc_trace_enabled())
        (void)bsxpc_log_dict(msg, MG_TRACE_PREFIX);
    if (msg == NULL || reply == NULL) return SVCHOST_ERR_BAD_MESSAGE;
    /* The root must be a dictionary; anything else (array, string,
     * endpoint) is malformed for this service. */
    if (xw_reader_root(msg, &root) != XW_OK) return SVCHOST_ERR_BAD_MESSAGE;
    if (root.type != XW_TYPE_DICTIONARY) return SVCHOST_ERR_BAD_MESSAGE;

    /* Handle NSXPC messages (bplist17 payload under "root"). */
    if (nsxpc_is_message(msg)) {
        nsxpc_request_t req;
        if (nsxpc_parse_request(msg, &req) == 0) {
            const char *sig = req.replysig;
            if (mg_strcmp(req.selector, "rebuildCache:") == 0) {
                (void)mg_ensure_cache();
                if (sig == NULL) sig = "v@?B";
                r = nsxpc_build_bool_reply_dict(reply, sig, 1);
                return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
            }
            if (mg_strcmp(req.selector, "setCacheSentinel:") == 0) {
                if (sig == NULL) sig = "v@?B";
                r = nsxpc_build_bool_reply_dict(reply, sig, 1);
                return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
            }
            if (mg_strcmp(req.selector, "getSpringboardRegionOverride:reply:") == 0) {
                if (sig == NULL) sig = "v@?@";
                r = nsxpc_build_nil_reply_dict(reply, sig);
                return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
            }
            if (mg_strcmp(req.selector, "getServerAnswerForQuestion:reply:") == 0) {
                if (sig == NULL) sig = "v@?@";
                if (req.has_question && req.question[0] != '\0') {
                    if (mg_answer_for(req.question, &ans) == 0) {
                        r = nsxpc_build_reply_dict(reply, sig, &ans);
                        return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
                    }
                    if (mg_is_protected(req.question) || mg_is_dynamic(req.question)) {
                        r = nsxpc_build_nil_reply_dict(reply, sig);
                        return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
                    }
                }
                r = nsxpc_build_nil_reply_dict(reply, sig);
                return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
            }
            /* Unknown selector: reply with nil answer */
            if (sig == NULL) sig = "v@?@";
            r = nsxpc_build_nil_reply_dict(reply, sig);
            return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
        } else {
            xw_value v;
            if (xw_dict_find(msg, "root", &v) == XW_OK)
                return SVCHOST_ERR_BAD_MESSAGE;
        }
    }

    /* First choice: the "question" key (plain-dict hypothesis). */
    {
        xw_value v;
        if (xw_dict_find(msg, MG_REQ_KEY_QUESTION, &v) == XW_OK) {
            const char *s = NULL;
            uint32_t slen = 0;
            if (xw_as_string(&v, &s, &slen) != XW_OK || s == NULL ||
                s[0] == '\0')
                return SVCHOST_ERR_BAD_MESSAGE;
            q = s;
        }
    }
    /* Fallback: any top-level string value naming a known question.
     * This is what lets the handler answer when the real NSXPC
     * invocation carries the question under a different key; the
     * verbatim trace above is what captures that shape. */
    if (q == NULL) {
        xw_dict_iter it;
        const char *k = NULL;
        xw_value item;
        int entries = 0;
        int nr = 0;
        if (xw_dict_iter_init(msg, &it) == XW_OK) {
            while ((nr = xw_dict_next(&it, &k, &item)) == 1) {
                const char *s = NULL;
                uint32_t slen = 0;
                entries++;
                if (xw_as_string(&item, &s, &slen) == XW_OK &&
                    s != NULL && s[0] != '\0' &&
                    (mg_answer_for(s, NULL) == 0 ||
                     mg_is_protected(s) || mg_is_dynamic(s))) {
                    q = s;
                    break;
                }
            }
            if (nr < 0) return SVCHOST_ERR_BAD_MESSAGE;
        }
        if (q == NULL && entries == 0) return SVCHOST_ERR_BAD_MESSAGE;
    }
    if (q == NULL) return SVCHOST_ERR_UNKNOWN_MESSAGE;
    if (mg_answer_for(q, &ans) == 0) {
        r = xw_dict_begin(reply);
        if (r == XW_OK) r = mg_add_answer_xw(reply, MG_REPLY_KEY_ANSWER,
                                            &ans);
        if (r == XW_OK) r = xw_dict_add_int64(reply, MG_REPLY_KEY_ERROR, 0);
        if (r == XW_OK) r = xw_dict_end(reply);
        if (r != XW_OK) return SVCHOST_ERR_NO_MEMORY;
        return 0;
    }
    if (mg_is_protected(q)) {
        /* Denied: NULL answer plus an error. Never a fabricated id. */
        if (mg_reply_null(reply, SVCHOST_ERR_UNKNOWN_MESSAGE) != XW_OK)
            return SVCHOST_ERR_NO_MEMORY;
        return 0;
    }
    if (mg_is_dynamic(q)) {
        /* No static answer: NULL answer, no error. */
        if (mg_reply_null(reply, 0) != XW_OK)
            return SVCHOST_ERR_NO_MEMORY;
        return 0;
    }
    return SVCHOST_ERR_UNKNOWN_MESSAGE;
}

int mg_init(void) {
    int kr;
    if (g_mg_inited) return KERN_SUCCESS;
    /* Best effort: the XPC service answers even when the cache file
     * cannot be written, so a cache failure does not fail init. */
    (void)mg_ensure_cache();
    kr = svchost_register(MG_SERVICE_NAME, mg_svchost_handler, NULL,
                          NULL);
    if (kr != KERN_SUCCESS) return kr;
    g_mg_inited = 1;
    return KERN_SUCCESS;
}

int mg_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                   uint8_t *reply_buf, uint32_t max_reply_size,
                   uint32_t *out_reply_size) {
    return svchost_process_msg(hdr, send_size, reply_buf,
                               max_reply_size, out_reply_size);
}
