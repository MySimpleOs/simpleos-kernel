#pragma once

#include <stdint.h>
#include <stddef.h>

/* Locate the initrd module handed to us by Limine and record an HHDM
 * pointer to its bytes. Returns zero on success, non-zero if no module
 * was supplied. */
int initrd_init(void);

/* Raw access to the mounted initrd for lower layers (VFS, debugger). */
const uint8_t *initrd_bytes(void);
size_t         initrd_size(void);
