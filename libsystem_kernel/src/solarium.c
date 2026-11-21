/*
 * solarium.c: Liquid Glass (Solarium) decision model. See solarium.h
 * for the rule and its evidence.
 *
 * Freestanding: integer logic only, no libc calls, no file I/O.
 */
#include "solarium.h"

int solarium_use_liquid_glass(uint32_t sdk_version,
                              int uid_design_requires_compat,
                              int flag,
                              int idiom_is_mac,
                              int hardware_supported)
{
    if (sdk_version < SOLARIUM_SDK_MIN) return 0;
    if (uid_design_requires_compat) return 0;
    if (flag != SOLARIUM_FLAG_ENABLED) return 0;
    if (idiom_is_mac) return 0;
    if (!hardware_supported) return 0;
    return 1;
}
