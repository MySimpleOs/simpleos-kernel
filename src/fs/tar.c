#include "tar.h"
#include "vfs.h"
#include "../kprintf.h"
#include "../mm/heap.h"

#include <stdint.h>
#include <stddef.h>

/* USTAR header layout — only the fields we care about.  Each record is
 * 512 bytes; file payloads follow in further 512-byte-aligned blocks. */
#define TAR_HDR_SIZE  512
#define TAR_NAME      0
#define TAR_SIZE      124
#define TAR_TYPEFLAG  156
#define TAR_PREFIX    345

static uint64_t oct_parse(const char *s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i] && s[i] != ' '; i++) {
        if (s[i] >= '0' && s[i] <= '7') v = v * 8 + (uint64_t) (s[i] - '0');
    }
    return v;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copy_name(char *dst, int cap, const char *src) {
    int i = 0;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static struct vnode *child_named(struct vnode *parent, const char *seg) {
    for (struct vnode *c = parent->children; c; c = c->next_sibling) {
        if (str_eq(c->name, seg)) return c;
    }
    return NULL;
}

static struct vnode *add_child(struct vnode *parent, const char *seg, int type) {
    struct vnode *c = kmalloc(sizeof(*c));
    if (!c) return NULL;
    copy_name(c->name, (int) sizeof(c->name), seg);
    c->type         = type;
    c->data         = NULL;
    c->size         = 0;
    c->parent       = parent;
    c->children     = NULL;
    c->next_sibling = parent->children;
    parent->children = c;
    return c;
}

static struct vnode *walk_or_make(const char *path, int leaf_type,
                                  const uint8_t *leaf_data, size_t leaf_size) {
    /* Skip leading "./" and "/" so "./etc/foo" and "/etc/foo" both resolve
     * to etc/foo under the root. */
    while (path[0] == '.' && path[1] == '/') path += 2;
    while (path[0] == '/') path++;
    if (!*path) return NULL;

    struct vnode *cur = vfs_root();
    while (*path) {
        char seg[VFS_NAME_MAX];
        int  i = 0;
        while (*path && *path != '/' && i < VFS_NAME_MAX - 1) {
            seg[i++] = *path++;
        }
        seg[i] = 0;
        int trailing_slash = (*path == '/');
        if (trailing_slash) path++;
        int is_last = (*path == 0);
        if (!seg[0]) continue;

        int seg_type = is_last ? leaf_type : VFS_DIR;
        struct vnode *next = child_named(cur, seg);
        if (!next) {
            next = add_child(cur, seg, seg_type);
            if (!next) return NULL;
        } else if (is_last && seg_type == VFS_FILE) {
            /* Re-encountering a file with the same name — just overwrite. */
            next->type = VFS_FILE;
        }

        if (is_last && leaf_type == VFS_FILE) {
            next->data = leaf_data;
            next->size = leaf_size;
        }
        cur = next;
    }
    return cur;
}

int tar_mount(const uint8_t *bytes, size_t len) {
    size_t off = 0;
    int    files = 0;
    int    dirs  = 0;

    while (off + TAR_HDR_SIZE <= len) {
        const char *h = (const char *) (bytes + off);

        /* End-of-archive is two consecutive zero blocks. A single zero-
         * filled header is enough of a hint that we can stop. */
        if (h[0] == 0) break;

        char fullpath[256];
        const char *name   = h + TAR_NAME;
        const char *prefix = h + TAR_PREFIX;
        if (prefix[0]) {
            int p = 0;
            while (p < 154 && prefix[p] && p < (int) sizeof(fullpath) - 2) {
                fullpath[p] = prefix[p]; p++;
            }
            fullpath[p++] = '/';
            int q = 0;
            while (name[q] && p < (int) sizeof(fullpath) - 1) {
                fullpath[p++] = name[q++];
            }
            fullpath[p] = 0;
        } else {
            copy_name(fullpath, (int) sizeof(fullpath), name);
        }

        uint64_t size = oct_parse(h + TAR_SIZE, 12);
        char     type = h[TAR_TYPEFLAG];
        const uint8_t *data = bytes + off + TAR_HDR_SIZE;

        if (type == '0' || type == 0) {
            walk_or_make(fullpath, VFS_FILE, data, (size_t) size);
            files++;
        } else if (type == '5') {
            walk_or_make(fullpath, VFS_DIR, NULL, 0);
            dirs++;
        }
        /* Silently ignore symlinks, device nodes, etc. for now. */

        off += TAR_HDR_SIZE + ((size + 511) & ~511ull);
    }

    kprintf("[tar] mounted %u files and %u directories from initrd\n",
            (unsigned) files, (unsigned) dirs);
    return 0;
}
