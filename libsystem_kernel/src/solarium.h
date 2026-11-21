/*
 * solarium.h: Liquid Glass (Solarium) decision model for ioswine.
 *
 * UIKit and SwiftUI decide per app whether to render
 * the iOS 26 floating Liquid Glass design or the legacy iOS 18 design.
 * This file pins the decision rule as reverse-engineered from the
 * iOS 26.6.2 (23G90) dyld shared cache,
 * so the rule is testable on host (tests/host/liquid_glass_test.c)
 * and so other components (notably the backdrop renderer) can ask
 * the same question UIKit asks.
 *
 * The rule:
 *
 * UIKit gate, __UISolariumEnabled (UIKitCore, 0x18914dcfc):
 *   Liquid Glass iff __UISolariumFeatureFlagEnabled AND the current
 *   device idiom is not Mac (UIUserInterfaceIdiomMac = 6).
 *   __UISolariumFeatureFlagEnabled is a cached call to
 *   -[UIDevice uncachedSolariumEnabledForCurrentDeviceIdiom].
 *
 * SwiftUI criteria, Solarium.EnablementCriteria (SwiftUICore):
 *   an OptionSet of {featureFlag, linkerCheck, hardwareSupport}.
 *   All three must hold, and the app must not be at the
 *   `compatibility` EnablementLevel (UIDesignRequiresCompatibility,
 *   or a pre-26 linked SDK).
 *     featureFlag: os_feature_enabled("SwiftUI", "Solarium") is true.
 *       The flag client (libsystem_featureflags) reads the plist
 *       files under /System/Library/FeatureFlags directly; there is
 *       no featureflagsd daemon. In 26.6.2 the flag is defined in
 *       Domain/SwiftUI.plist as {DevelopmentPhase: FeatureComplete,
 *       DisclosureRequired: 11ef4753-...}, and that UUID is marked
 *       Disclosed in GlobalDisclosures.plist. A flag with no Enabled
 *       key is true iff DevelopmentPhase is FeatureComplete; a flag
 *       with no plist data at all (missing dir) is false.
 *     linkerCheck: the app's linked SDK is >= iOS 26.0
 *       (dyld_program_sdk_at_least(26.0)). Our loader reports SDK
 *       26 for every app, so this leg passes here.
 *     hardwareSupport: the device is not a low-performance device
 *       (__UISolariumIsLowPerformanceDevice is false); a
 *       com.apple.SwiftUI.IgnoreSolariumHardwareCheck default
 *       overrides it. iPhone17,3 is not low-performance, so this
 *       leg passes for the test phone's emulated device.
 *
 * So, in one predicate, Liquid Glass iff ALL of:
 *     (a) the app's linked SDK is >= iOS 26.0 (0x001a0000), AND
 *     (b) the app's Info.plist does NOT set UIDesignRequiresCompatibility, AND
 *     (c) the Solarium feature flag is ENABLED (not merely present), AND
 *     (d) the device idiom is not Mac, AND
 *     (e) the device has hardware support (not low-performance).
 *   When the flag is unknown (no FeatureFlags dir in the sysroot),
 *   the answer is legacy: an unknown flag is a disabled flag. That
 *   is the device observation: Aidoku is SDK-27.1-linked with
 *   no compatibility key, yet renders legacy, because the sysroot
 *   has no FeatureFlags dir at all.
 *
 * Freestanding: integer logic only, no libc, no file I/O. The flag
 * value itself comes from the feature-flags layer (see featureflags.c
 * when it exists); this file only combines the inputs.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>

/* Minimum linked SDK for Liquid Glass: iOS 26.0, as a dyld version
 * token (major << 16 | minor << 8 | patch). */
#define SOLARIUM_SDK_MIN 0x001a0000u

/* Feature flag states. UNKNOWN means no FeatureFlags data was found
 * for the flag (missing dir, missing file, no daemon answer). */
#define SOLARIUM_FLAG_UNKNOWN  (-1)
#define SOLARIUM_FLAG_DISABLED 0
#define SOLARIUM_FLAG_ENABLED  1

/* Return 1 when the app gets the Liquid Glass design, 0 for legacy.
 * sdk_version is the dyld program SDK version token (what
 * dyld_get_program_sdk_version returns). uid_design_requires_compat
 * is nonzero when the app's Info.plist sets UIDesignRequiresCompatibility
 * to true. flag is SOLARIUM_FLAG_* for the Solarium feature flag.
 * idiom_is_mac is nonzero when the current device idiom is Mac
 * (UIUserInterfaceIdiomMac = 6, e.g. Mac Catalyst). hardware_supported
 * is nonzero when the device passes the hardware-support leg
 * (not a low-performance device). */
int solarium_use_liquid_glass(uint32_t sdk_version,
                              int uid_design_requires_compat,
                              int flag,
                              int idiom_is_mac,
                              int hardware_supported);
