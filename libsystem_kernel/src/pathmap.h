#pragma once
#include <stddef.h>

#define LSK_PATH_MAX 1024

/*
 * Remap an iOS root-filesystem path (e.g. /System/..., /usr/share/...,
 * /Library/...) to <IOSWINE_SYSROOT><in> when the remapped path exists on
 * the host/device filesystem. Returns `in` unchanged if not remappable,
 * if the remapped path does not exist, or if `buf` is too small.
 */
const char *lsk_path_remap(const char *in, char *buf, size_t cap);

/*
 * Reverse-remap an in-place buffer (e.g. from F_GETPATH) if it begins with
 * <IOSWINE_SYSROOT> followed by one of the remappable prefixes.
 */
void lsk_path_unmap_inplace(char *path);
