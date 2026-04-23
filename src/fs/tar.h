#pragma once

#include <stdint.h>
#include <stddef.h>

/* Parse a USTAR archive in memory and splice every regular file into the
 * VFS tree under the root, creating intermediate directories on the fly. */
int tar_mount(const uint8_t *bytes, size_t len);
