/*
 * liquid_glass_test.c - Host unit test for the Solarium (Liquid Glass)
 * decision model (libsystem_kernel/src/solarium.c).
 *
 * The decision rule:
 *   Liquid Glass iff linked SDK >= 26.0 AND no
 *   UIDesignRequiresCompatibility AND the Solarium feature flag is
 *   ENABLED AND the device idiom is not Mac AND the device has
 *   hardware support (not low-performance). An unknown flag (no
 *   FeatureFlags data) is legacy, not glass: that is the 021 device
 *   observation (Aidoku is SDK-27.1 linked with no compatibility
 *   key, yet renders the legacy tab bar, because the sysroot has no
 *   FeatureFlags dir).
 *
 * Version tokens are dyld SDK versions: major << 16 | minor << 8 |
 * patch. 0x001a0000 = 26.0.0, 0x001b0000 = 27.0.0 (Aidoku's linked
 * SDK is 27.1, i.e. 0x001b0100), 0x00190000 = 25.0.0.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include "solarium.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* One decision case: expected 1 is Liquid Glass, 0 is legacy.
 * idiom_mac is nonzero for Mac idiom; hw is nonzero when the
 * device has hardware support. */
static void check_case(uint32_t sdk, int compat, int flag, int idiom_mac,
                       int hw, int want, const char *name)
{
    int got = solarium_use_liquid_glass(sdk, compat, flag, idiom_mac, hw);
    (void)name;
    CHECK(got == want);
}

int main(void)
{
    /* The Aidoku case: SDK 27.1, no compat key, flag enabled,
     * phone idiom, hardware supported. */
    check_case(0x001b0100u, 0, SOLARIUM_FLAG_ENABLED, 0, 1, 1, "aidoku-glass");
    /* The 021 device observation: same app, flag unknown -> legacy. */
    check_case(0x001b0100u, 0, SOLARIUM_FLAG_UNKNOWN, 0, 1, 0, "aidoku-legacy-noflag");
    /* Flag explicitly disabled -> legacy even for a 27.1 app. */
    check_case(0x001b0100u, 0, SOLARIUM_FLAG_DISABLED, 0, 1, 0, "aidoku-legacy-disabled");
    /* The loader reports SDK 26.0.0 for every app: that alone passes
     * the SDK leg, so with the flag enabled the answer is glass. */
    check_case(0x001a0000u, 0, SOLARIUM_FLAG_ENABLED, 0, 1, 1, "loader-sdk26-glass");
    /* ... but with the flag unknown the loader's SDK 26 still gives
     * legacy: the SDK leg is not the blocker on device. */
    check_case(0x001a0000u, 0, SOLARIUM_FLAG_UNKNOWN, 0, 1, 0, "loader-sdk26-legacy-noflag");
    /* SDK boundary: 26.0.0 is in, 25.9.9 is out. */
    check_case(0x001a0000u, 0, SOLARIUM_FLAG_ENABLED, 0, 1, 1, "sdk-26.0-in");
    check_case(0x00190900u, 0, SOLARIUM_FLAG_ENABLED, 0, 1, 0, "sdk-25.9-out");
    check_case(0x00190000u, 0, SOLARIUM_FLAG_ENABLED, 0, 1, 0, "sdk-25.0-out");
    /* Compatibility key opts out even with SDK 27.1 and flag on. */
    check_case(0x001b0100u, 1, SOLARIUM_FLAG_ENABLED, 0, 1, 0, "compat-opts-out");
    check_case(0x001a0000u, 1, SOLARIUM_FLAG_ENABLED, 0, 1, 0, "compat-opts-out-26");
    /* Mac idiom (UIUserInterfaceIdiomMac = 6, e.g. Mac Catalyst)
     * is legacy even with every other leg passing: the UIKit gate
     * (__UISolariumEnabled) requires idiom != Mac. */
    check_case(0x001b0100u, 0, SOLARIUM_FLAG_ENABLED, 1, 1, 0, "mac-idiom-legacy");
    check_case(0x001a0000u, 0, SOLARIUM_FLAG_ENABLED, 1, 1, 0, "mac-idiom-legacy-26");
    /* No hardware support (low-performance device) is legacy even
     * with every other leg passing: the SwiftUI criteria require
     * hardwareSupport. */
    check_case(0x001b0100u, 0, SOLARIUM_FLAG_ENABLED, 0, 0, 0, "no-hw-legacy");
    check_case(0x001a0000u, 0, SOLARIUM_FLAG_ENABLED, 0, 0, 0, "no-hw-legacy-26");
    /* Every input off at once is legacy (no accidental glass). */
    check_case(0x00190000u, 1, SOLARIUM_FLAG_DISABLED, 1, 0, 0, "all-off");
    check_case(0x00190000u, 1, SOLARIUM_FLAG_UNKNOWN, 1, 0, 0, "all-off-unknown");
    /* SDK above 27 (future) still passes the leg. */
    check_case(0x001c0000u, 0, SOLARIUM_FLAG_ENABLED, 0, 1, 1, "sdk-28-glass");
    /* A flag value outside the documented set is not enabled. */
    check_case(0x001b0100u, 0, 2, 0, 1, 0, "flag-2-not-enabled");
    check_case(0x001b0100u, 0, -2, 0, 1, 0, "flag-minus2-not-enabled");

    printf("liquid_glass_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
