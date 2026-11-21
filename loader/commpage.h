/* Darwin commpage at 0xfffffc000 (ARCHITECTURE.md §2, §12). */
#pragma once
#include <stdint.h>
#define COMMPAGE_BASE 0xfffffc000ull
#define COMMPAGE_SIZE 0x4000ull
/* Map and fill the commpage, then make it read-only. ncpus from the host. Dies on error. */
void commpage_setup(void);
