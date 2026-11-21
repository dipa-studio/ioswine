#pragma once
#include <stdint.h>
#include <stddef.h>

void set_errno(int err);
int linux_to_darwin_errno(long linux_err);
