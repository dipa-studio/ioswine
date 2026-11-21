/* dyld-light (libSystem mode). */
#pragma once
#include <stdint.h>
/* Prepare everything on the loader stack. Returns exe main; *boot_fn = function to call on the Darwin
   stack as boot(argc, argv, envp, apple) (it runs initializers, main and exit). */
uint64_t dyld_setup(const char *exe_path, const char *dsc_path, char **envp, uint64_t *boot_fn);
/* Install the fake dyld4::gAPIs object (loader/dyld_apis.c). */
void dyld_apis_setup(void);
/* Loaded image list (index 0 = main executable). */
uint32_t dyld_image_count(void);
uint64_t dyld_image_mh(uint32_t i);
const char *dyld_image_path(uint32_t i);
/* After libSystem init: call objc's init (+load) callback for every image, if objc registered. */
void dyld_objc_run_inits(void);
uint64_t dyld_dlopen(const char *path);
uint64_t dyld_dlsym(uint64_t handle, const char *name);
/* objc: notify mapped (+ init for +load images) for images [from, count) added after registration. */
void dyld_objc_notify_new(uint32_t from);

/* Dry-run load plan generator. Outputs plan to stdout and returns. */
void dyld_print_plan(const char *target, const char *dsc_path, char **envp);
