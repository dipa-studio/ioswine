/* dyld shared cache mapping, slide info v5 + PAC re-signing, image/export lookup
   (ARCHITECTURE.md §8, §12). */
#pragma once
#include <stdint.h>

/* Map the main cache file and all subcaches at their unslid addresses and apply slide info.
   path = main cache file (subcaches are path + suffix). Dies on error. */
void dsc_load(const char *path);

/* Mach header address of a cache image by install name, 0 if absent. */
uint64_t dsc_image(const char *install_name);

/* Look up an exported symbol (Mach-O name, with leading '_') in image mh, following re-exports.
   Returns the address (unsigned, unauthenticated), 0 if not found. */
uint64_t dsc_lookup(uint64_t mh, const char *sym);

/* Statistics from the last dsc_load. */
extern uint64_t dsc_stat_plain, dsc_stat_auth;
/* A few signed pointers recorded during rebase, for the self-test. */
#define DSC_NSAMPLES 16
struct dsc_auth_sample { uint64_t loc, target, disc; unsigned key; };
extern struct dsc_auth_sample dsc_samples[DSC_NSAMPLES];
extern int dsc_nsamples;

/* n-th (1-based) dependent dylib install name of image mh, 0 if none. */
const char *dsc_dep_name(uint64_t mh, uint64_t ordinal);
/* PAC metadata of the cache auth pointer stored at addr (from slide info). 1 = auth pointer found. */
int dsc_auth_meta(uint64_t addr, uint16_t *div, int *addr_div, unsigned *key);
/* Install name of the cache image whose mach header is at mh, 0 if none. */
const char *dsc_image_path(uint64_t mh);

/* Lookup without alias fallback. */
uint64_t dsc_image_raw(const char *name);

struct dsc_alias_entry {
    char from[128];
    char to[128];
};
int dsc_get_aliases(const struct dsc_alias_entry **out_entries, int *out_count);
const char *dsc_resolve_alias(const char *name);
