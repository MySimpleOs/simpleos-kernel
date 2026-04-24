#include "shadow.h"
#include "surface.h"

#include "../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

/* ---- integer sqrt ---------------------------------------------------------
 *
 * Newton-free bitwise sqrt — 32 iterations at most, branch-predictable on
 * the shadow-band pixel count we care about (roughly 2πr per corner). No
 * FPU needed, which matters because shadow runs inside the general-regs
 * kernel TU and SSE hasn't been enabled yet when heap init calls this.
 */
static uint32_t isqrt_u32(uint32_t x) {
    uint32_t r = 0;
    uint32_t bit;
    bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        uint32_t rb = r + bit;
        if (x >= rb) { x -= rb; r = (r >> 1) + bit; }
        else         { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

unsigned char shadow_corner_mask(int lx, int ly, int w, int h, int r) {
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) return 0;
    if (r <= 0) return 255;

    int in_left  = lx <  r;
    int in_right = lx >= w - r;
    int in_top   = ly <  r;
    int in_bot   = ly >= h - r;

    /* Safe middle band: full opacity, no math. */
    if (!((in_left || in_right) && (in_top || in_bot))) return 255;

    /* Corner arc centers. For a radius-r arc in the top-left corner, the
     * center sits at (r - 0.5, r - 0.5) so that (0,0) is exactly on the
     * outer edge. Work in 16ths of a pixel to get clean AA without
     * floats: distances and radii all scale by 16 at once. */
    int ccx16 = in_left ? (2 * r - 1) * 8 : (2 * (w - r) + 1) * 8;
    int ccy16 = in_top  ? (2 * r - 1) * 8 : (2 * (h - r) + 1) * 8;

    /* Pixel center in 16ths: (lx + 0.5) * 16 = lx*16 + 8. */
    int px16 = lx * 16 + 8;
    int py16 = ly * 16 + 8;

    int dx16 = px16 - ccx16;
    int dy16 = py16 - ccy16;
    uint32_t d2 = (uint32_t) (dx16 * dx16 + dy16 * dy16);
    uint32_t d16 = isqrt_u32(d2);
    uint32_t r16 = (uint32_t) r * 16u;

    /* 1-pixel AA band centered on the ideal radius. Outside the band,
     * clamp to 0 / 255; inside, linearly interpolate. */
    if (d16 + 8u <= r16)       return 255;
    if (d16 >= r16 + 8u)       return 0;
    /* mask = 255 * ((r16 + 8) - d16) / 16 */
    uint32_t num = (r16 + 8u) - d16;     /* in [0, 16]                */
    uint32_t m   = (num * 255u + 8u) / 16u;
    if (m > 255u) m = 255u;
    return (unsigned char) m;
}

/* ---- horizontal box blur, one row at a time -----------------------------
 *
 * Classic running-sum sliding window. For radius R the filter width is
 * 2R+1, and the sum is initialised to cover [-R..R] assuming samples
 * outside the buffer are zero. Each step adds the incoming pixel at
 * (x+R) and drops the outgoing one at (x-R-1).
 */
static void blur_row_horz(const uint8_t *in, uint8_t *out,
                          int w, int R) {
    int kw = 2 * R + 1;
    int sum = 0;
    for (int x = 0; x <= R && x < w; x++) sum += in[x];
    for (int x = 0; x < w; x++) {
        int add = x + R;
        int sub = x - R - 1;
        if (add < w)  sum += in[add];
        if (sub >= 0) sum -= in[sub];
        /* Sum is always non-negative; the running update can never make
         * it > kw * 255 < 2^16, so uint8 division is fine. */
        out[x] = (uint8_t) (sum / kw);
    }
}

/* Vertical pass — same idea but strides through columns. */
static void blur_col_vert(uint8_t *buf, uint8_t *scratch,
                          int w, int h, int R) {
    int kw = 2 * R + 1;
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int y = 0; y <= R && y < h; y++) sum += buf[y * w + x];
        for (int y = 0; y < h; y++) {
            int add = y + R;
            int sub = y - R - 1;
            if (add < h)  sum += buf[add * w + x];
            if (sub >= 0) sum -= buf[sub * w + x];
            scratch[y * w + x] = (uint8_t) (sum / kw);
        }
    }
    /* Copy scratch back into buf so the caller sees the result in-place. */
    size_t n = (size_t) w * (size_t) h;
    for (size_t i = 0; i < n; i++) buf[i] = scratch[i];
}

/* One full box-blur pass, separable (horz+vert). The caller does three of
 * these to approximate a Gaussian; 3 passes matches the well-known "stack
 * blur" close-enough-to-gaussian sweet spot. */
static void blur_pass(uint8_t *buf, uint8_t *scratch, int w, int h, int R) {
    if (R <= 0) return;
    /* Horizontal → write into scratch, then copy back. Keeping the two
     * stages on separate buffers avoids write-read aliasing on the
     * sliding window's add side. */
    for (int y = 0; y < h; y++) {
        blur_row_horz(buf + y * w, scratch + y * w, w, R);
    }
    size_t n = (size_t) w * (size_t) h;
    for (size_t i = 0; i < n; i++) buf[i] = scratch[i];
    blur_col_vert(buf, scratch, w, h, R);
}

void shadow_regen(struct surface *s) {
    if (!s) return;
    if (s->shadow_blur == 0 || s->shadow_alpha == 0) {
        if (s->shadow_mask) {
            kfree(s->shadow_mask);
            s->shadow_mask = NULL;
            s->shadow_mask_w = 0;
            s->shadow_mask_h = 0;
        }
        return;
    }

    int R  = (int) s->shadow_blur;
    int mw = (int) s->width  + 2 * R;
    int mh = (int) s->height + 2 * R;
    if (mw <= 0 || mh <= 0) return;

    size_t bytes = (size_t) mw * (size_t) mh;

    /* Resize if the cached buffer no longer matches. */
    if (s->shadow_mask &&
        (s->shadow_mask_w != (uint32_t) mw ||
         s->shadow_mask_h != (uint32_t) mh)) {
        kfree(s->shadow_mask);
        s->shadow_mask = NULL;
    }
    if (!s->shadow_mask) {
        s->shadow_mask = (uint8_t *) kmalloc(bytes);
        if (!s->shadow_mask) { s->shadow_mask_w = s->shadow_mask_h = 0; return; }
        s->shadow_mask_w = (uint32_t) mw;
        s->shadow_mask_h = (uint32_t) mh;
    }

    /* Silhouette pass: rounded-rect mask inset by R pixels into the
     * padded buffer. Everything outside the surface proper is zeroed,
     * which naturally produces the feathered halo once the blur runs. */
    uint8_t *m = s->shadow_mask;
    int sw = (int) s->width;
    int sh = (int) s->height;
    int cr = (int) s->corner_radius;
    for (int y = 0; y < mh; y++) {
        int ly = y - R;
        uint8_t *row = m + (size_t) y * mw;
        for (int x = 0; x < mw; x++) {
            int lx = x - R;
            row[x] = shadow_corner_mask(lx, ly, sw, sh, cr);
        }
    }

    /* Scratch buffer for the two-pass separable blur. One allocation,
     * same size as the mask. */
    uint8_t *scratch = (uint8_t *) kmalloc(bytes);
    if (!scratch) return;  /* silhouette alone still looks OK */

    /* Three small passes ≈ one wider Gaussian. Each radius r' satisfies
     * r' ≈ blur/3 so the total variance matches a single pass of radius
     * `blur` within a few percent. Picking floor(R/3) with the remainder
     * distributed across the first one or two passes keeps the total
     * kernel width exactly 2R+1. */
    int r1 = R / 3;
    int r2 = R / 3;
    int r3 = R - r1 - r2;
    blur_pass(m, scratch, mw, mh, r1);
    blur_pass(m, scratch, mw, mh, r2);
    blur_pass(m, scratch, mw, mh, r3);

    kfree(scratch);
}
