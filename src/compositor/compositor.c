#include "compositor.h"
#include "surface.h"
#include "blit.h"

#include "../gpu/display.h"
#include "../kprintf.h"

#include <stdint.h>
#include <stddef.h>

static struct surface *slots[COMPOSITOR_MAX_SURFACES];
static int             slot_count;

void compositor_init(void) {
    for (int i = 0; i < COMPOSITOR_MAX_SURFACES; i++) slots[i] = NULL;
    slot_count = 0;
    kprintf("[compositor] init, %d surface slots\n", COMPOSITOR_MAX_SURFACES);
}

int compositor_add(struct surface *s) {
    if (!s) return -1;
    for (int i = 0; i < slot_count; i++) {
        if (slots[i] == s) return 0;
    }
    if (slot_count >= COMPOSITOR_MAX_SURFACES) return -1;
    slots[slot_count++] = s;
    return 0;
}

void compositor_remove(struct surface *s) {
    for (int i = 0; i < slot_count; i++) {
        if (slots[i] == s) {
            for (int j = i; j < slot_count - 1; j++) slots[j] = slots[j + 1];
            slots[--slot_count] = NULL;
            return;
        }
    }
}

static int top_z(void) {
    int32_t max_z = 0;
    int     have  = 0;
    for (int i = 0; i < slot_count; i++) {
        if (!have || slots[i]->z > max_z) { max_z = slots[i]->z; have = 1; }
    }
    return have ? max_z : 0;
}

static int bottom_z(void) {
    int32_t min_z = 0;
    int     have  = 0;
    for (int i = 0; i < slot_count; i++) {
        if (!have || slots[i]->z < min_z) { min_z = slots[i]->z; have = 1; }
    }
    return have ? min_z : 0;
}

void compositor_raise(struct surface *s) {
    if (!s) return;
    s->z = top_z() + 1;
}

void compositor_lower(struct surface *s) {
    if (!s) return;
    s->z = bottom_z() - 1;
}

/* Insertion-sort slot indices by z ascending (low → high). Stable enough
 * for <=64 entries and avoids pulling in a qsort dep. */
static void sort_indices(int *idx) {
    for (int i = 0; i < slot_count; i++) idx[i] = i;
    for (int i = 1; i < slot_count; i++) {
        int key = idx[i];
        int32_t key_z = slots[key]->z;
        int j = i - 1;
        while (j >= 0 && slots[idx[j]]->z > key_z) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }
}

void compositor_frame(uint32_t bg_xrgb) {
    const struct display *dd = display_get();
    if (!dd || !dd->pixels) return;

    struct blit_dst dst = {
        .pixels = dd->pixels,
        .width  = dd->width,
        .height = dd->height,
        .stride = dd->pitch / 4u,
    };

    blit_fill(&dst, 0, 0, (int32_t) dd->width, (int32_t) dd->height, bg_xrgb);

    int idx[COMPOSITOR_MAX_SURFACES];
    sort_indices(idx);

    for (int i = 0; i < slot_count; i++) {
        struct surface *s = slots[idx[i]];
        if (!s || !s->pixels || !s->visible || s->alpha == 0) continue;

        struct blit_src src = {
            .pixels = s->pixels,
            .width  = s->width,
            .height = s->height,
            .stride = s->width,
        };
        if (s->opaque) blit_copy(&dst, s->x, s->y, &src);
        else           blit_alpha(&dst, s->x, s->y, &src, s->alpha);
    }

    display_present();
}
