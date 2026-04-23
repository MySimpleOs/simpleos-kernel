#include "vfs.h"
#include "../kprintf.h"
#include "../mm/heap.h"

#include <stdint.h>
#include <stddef.h>

static struct vnode root_node;

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copy_name(char *dst, const char *src) {
    int i = 0;
    while (i < VFS_NAME_MAX - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void vfs_init(void) {
    copy_name(root_node.name, "/");
    root_node.type         = VFS_DIR;
    root_node.data         = NULL;
    root_node.size         = 0;
    root_node.parent       = NULL;
    root_node.children     = NULL;
    root_node.next_sibling = NULL;
}

struct vnode *vfs_root(void) {
    return &root_node;
}

struct vnode *vfs_lookup(const char *path) {
    if (!path || *path != '/') return NULL;
    path++;  /* skip leading '/' */
    struct vnode *cur = &root_node;

    while (*path) {
        char seg[VFS_NAME_MAX];
        int  i = 0;
        while (*path && *path != '/' && i < VFS_NAME_MAX - 1) {
            seg[i++] = *path++;
        }
        seg[i] = 0;
        if (*path == '/') path++;
        if (!seg[0]) continue;   /* "//" or trailing '/'                   */

        if (cur->type != VFS_DIR) return NULL;

        struct vnode *child = cur->children;
        while (child && !str_eq(child->name, seg)) child = child->next_sibling;
        if (!child) return NULL;
        cur = child;
    }
    return cur;
}

int64_t vfs_read(struct vnode *v, size_t offset, size_t count, void *buf) {
    if (!v || v->type != VFS_FILE) return -1;
    if (offset >= v->size) return 0;
    if (offset + count > v->size) count = v->size - offset;
    uint8_t *dst = buf;
    for (size_t i = 0; i < count; i++) dst[i] = v->data[offset + i];
    return (int64_t) count;
}

static const char *type_str(int t) {
    return t == VFS_DIR ? "dir" : t == VFS_FILE ? "file" : "?";
}

void vfs_dump(const struct vnode *v, int depth) {
    if (!v) v = &root_node;
    for (int i = 0; i < depth; i++) kprintf("  ");
    if (v->type == VFS_FILE) {
        kprintf("%s (file, %u bytes)\n", v->name, (unsigned) v->size);
    } else {
        kprintf("%s (%s)\n", v->name, type_str(v->type));
    }
    for (struct vnode *c = v->children; c; c = c->next_sibling) {
        vfs_dump(c, depth + 1);
    }
}
