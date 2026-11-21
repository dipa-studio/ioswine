/* Swap the cache's libsystem_kernel for our dylib. */
#pragma once
#include <stdint.h>
/* our_mh: mach header of our loaded libsystem_kernel.dylib. Dies if a code export has no replacement. */
void lsk_swap(uint64_t our_mh, uint64_t *out_patched);
