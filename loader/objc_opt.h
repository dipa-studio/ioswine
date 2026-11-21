/* Lookups in the shared cache's prebuilt ObjC hash tables (ObjCOptimizationHeader at 0x1fa261610 for 23G90).
   Implementation loader/objc_opt.c. Freestanding, loader runtime only. */
#pragma once
#include <stdint.h>
/* Calls cb(cls, ctx) for every class named `name` in the cache class table (duplicates included), in table order,
   until cb returns nonzero. Returns the number of classes visited. cls = runtime address of the objc class. */
int objc_opt_find_class(const char *name, int (*cb)(uint64_t cls, void *ctx), void *ctx);
uint32_t objc_opt_class_count(void);
/* Address of the unique selector string for `name` in the cache selector table, 0 if absent/no table. */
uint64_t objc_opt_find_selector(const char *name);
/* Calls cb for every protocol named `name` in the cache protocol table (same contract as classes). */
int objc_opt_find_protocol(const char *name, int (*cb)(uint64_t proto, void *ctx), void *ctx);
