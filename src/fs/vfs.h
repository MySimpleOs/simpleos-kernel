#pragma once

#include <stdint.h>
#include <stddef.h>

#define VFS_NAME_MAX 64

enum { VFS_DIR = 1, VFS_FILE = 2 };

struct vnode {
    char  name[VFS_NAME_MAX];
    int   type;
    /* For files: points into the mounted image. For dirs: NULL. */
    const uint8_t *data;
    size_t size;

    struct vnode *parent;
    struct vnode *children;      /* head of linked list                    */
    struct vnode *next_sibling;
};

void           vfs_init(void);
struct vnode  *vfs_root(void);

/* Resolve an absolute path ("/etc/foo") to its vnode, or NULL. */
struct vnode  *vfs_lookup(const char *path);

/* Copy up to `count` bytes from offset into buf. Returns bytes read, 0 at
 * EOF, -1 on type error. */
int64_t        vfs_read(struct vnode *v, size_t offset, size_t count, void *buf);

/* Recursive serial log of the tree under `v` (NULL means root). Useful for
 * boot-time smoke tests. */
void           vfs_dump(const struct vnode *v, int depth);
