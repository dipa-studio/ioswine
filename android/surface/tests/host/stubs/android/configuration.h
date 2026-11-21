/*
 * Host-test stub for <android/configuration.h>.
 * Only the orientation query server.c uses is stubbed.
 */
#ifndef TEST_STUB_ANDROID_CONFIGURATION_H
#define TEST_STUB_ANDROID_CONFIGURATION_H

#include <stdint.h>

typedef struct AConfiguration AConfiguration;

#define ACONFIGURATION_ORIENTATION_ANY 0
#define ACONFIGURATION_ORIENTATION_PORT 1
#define ACONFIGURATION_ORIENTATION_LAND 2
#define ACONFIGURATION_ORIENTATION_SQUARE 3

struct AConfiguration {
    int32_t orientation;
};

int32_t AConfiguration_getOrientation(AConfiguration *config);

/* Test helper (implemented in test_server_stubs.c). */
AConfiguration *test_config_create(int32_t orientation);

#endif
