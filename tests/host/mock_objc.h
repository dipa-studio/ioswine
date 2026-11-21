/*
 * mock_objc.h: plain C mock ObjC runtime for the metaldev host test.
 *
 * Implements struct mtl_objc_runtime over a registry table (an array
 * of class entries, each with a method list). No Apple headers. The
 * class pointer handed back for a class is the address of its registry
 * entry: stable, unique, and comparable with ==.
 *
 * mock_runtime() returns a struct mtl_objc_runtime wired to the mock
 * functions; pass it to metaldev_init_with_runtime(). The test asserts
 * on the registry through mock_get_class() (NULL unless the class was
 * registered) and mock_lookup_method() (the IMP for a class+selector,
 * or NULL when the method was never registered).
 *
 * Usage notes: mock_reset() clears the registry; call it once at the
 * start of the test. allocate_class_pair rejects a duplicate class
 * name and class_add_method rejects a duplicate selector, like the
 * real runtime would. extra_bytes and the type-encoding string are
 * accepted and ignored: the skeleton objects have fixed layouts and
 * the test checks registration, not encodings.
 */
#pragma once

#include <stddef.h>
#include "metaldev.h"

#define MOCK_OBJC_MAX_CLASSES 16
#define MOCK_OBJC_MAX_METHODS 32

struct mock_method {
    const char *sel;
    void *imp;
};

struct mock_class {
    const char *name;
    unsigned int registered;
    unsigned int method_count;
    struct mock_method methods[MOCK_OBJC_MAX_METHODS];
};

static struct mock_class g_mock_classes[MOCK_OBJC_MAX_CLASSES];
static unsigned int g_mock_class_count;

static int mock_streq(const char *a, const char *b) {
    unsigned int i = 0;
    for (;;) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
        i++;
    }
}

static struct mock_class *mock_find_class_by_ptr(void *cls) {
    unsigned int i;
    for (i = 0; i < g_mock_class_count; i++) {
        if ((void *)&g_mock_classes[i] == cls) {
            return &g_mock_classes[i];
        }
    }
    return NULL;
}

static struct mock_class *mock_find_class_by_name(const char *name) {
    unsigned int i;
    for (i = 0; i < g_mock_class_count; i++) {
        if (g_mock_classes[i].name != NULL &&
            mock_streq(g_mock_classes[i].name, name)) {
            return &g_mock_classes[i];
        }
    }
    return NULL;
}

static void *mock_allocate_class_pair(void *superclass, const char *name,
                                      unsigned long extra_bytes) {
    struct mock_class *c;
    (void)superclass;
    (void)extra_bytes;
    if (name == NULL) {
        return NULL;
    }
    if (mock_find_class_by_name(name) != NULL) {
        return NULL;
    }
    if (g_mock_class_count >= MOCK_OBJC_MAX_CLASSES) {
        return NULL;
    }
    c = &g_mock_classes[g_mock_class_count++];
    c->name = name;
    c->registered = 0;
    c->method_count = 0;
    return (void *)c;
}

static int mock_class_add_method(void *cls, const char *sel, void *imp,
                                 const char *types) {
    struct mock_class *c = mock_find_class_by_ptr(cls);
    unsigned int i;
    (void)types;
    if (c == NULL || sel == NULL || imp == NULL) {
        return -1;
    }
    for (i = 0; i < c->method_count; i++) {
        if (mock_streq(c->methods[i].sel, sel)) {
            return -1;
        }
    }
    if (c->method_count >= MOCK_OBJC_MAX_METHODS) {
        return -1;
    }
    c->methods[c->method_count].sel = sel;
    c->methods[c->method_count].imp = imp;
    c->method_count++;
    return 0;
}

static void mock_register_class_pair(void *cls) {
    struct mock_class *c = mock_find_class_by_ptr(cls);
    if (c != NULL) {
        c->registered = 1;
    }
}

static void *mock_get_class(const char *name) {
    struct mock_class *c;
    if (name == NULL) {
        return NULL;
    }
    c = mock_find_class_by_name(name);
    if (c == NULL || !c->registered) {
        return NULL;
    }
    return (void *)c;
}

/* Retrieve a registered method's IMP by class+selector, or NULL. */
static void *mock_lookup_method(void *cls, const char *sel) {
    struct mock_class *c = mock_find_class_by_ptr(cls);
    unsigned int i;
    if (c == NULL || sel == NULL) {
        return NULL;
    }
    for (i = 0; i < c->method_count; i++) {
        if (mock_streq(c->methods[i].sel, sel)) {
            return c->methods[i].imp;
        }
    }
    return NULL;
}

static unsigned int mock_class_count(void) {
    return g_mock_class_count;
}

static void mock_reset(void) {
    unsigned int i;
    for (i = 0; i < MOCK_OBJC_MAX_CLASSES; i++) {
        g_mock_classes[i].name = NULL;
        g_mock_classes[i].registered = 0;
        g_mock_classes[i].method_count = 0;
    }
    g_mock_class_count = 0;
}

static struct mtl_objc_runtime mock_runtime(void) {
    struct mtl_objc_runtime rt;
    rt.allocate_class_pair = mock_allocate_class_pair;
    rt.class_add_method = mock_class_add_method;
    rt.register_class_pair = mock_register_class_pair;
    rt.get_class = mock_get_class;
    return rt;
}
