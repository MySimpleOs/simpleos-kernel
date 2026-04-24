/* SIMD row fast-paths for the compositor. Compiled WITHOUT
 * -mgeneral-regs-only (see the per-file rule in kernel/Makefile) so GCC
 * can emit XMM/YMM and <immintrin.h> intrinsics lower to real ops. The
 * rest of the kernel stays general-regs-only — that's why these kernels
 * live in their own translation unit behind the row-level signatures
 * declared in blit.h.
 *
 * Per-function __attribute__((target(...))) is what keeps SSE2 and AVX2
 * encodings separate. Without it, a file-level -mavx2 convinces GCC to
 * emit VEX-prefixed forms of SSE2 intrinsics too, which then crash with
 * #UD on a CPU that has SSE2 but no AVX — e.g. QEMU's default qemu64.
 * Marking each function individually ties its entire body (intrinsics
 * and the scalar-C it compiles into) to exactly that ISA baseline.
 *
 * Callers (blit.c) guarantee:
 *   - simd_cpu_init() has enabled SSE / AVX on this CPU,
 *   - the row has already been clipped — both dst and src hold `n`
 *     valid pixels, `n >= 0`.
 *
 * Math matches the scalar paths byte-for-byte so switching between SSE2,
 * AVX2 and scalar produces the same framebuffer.
 */

#include "blit.h"

/* GCC's <immintrin.h> pulls in <mm_malloc.h> → <stdlib.h>, which a
 * freestanding kernel does not provide. The guard is published: define it
 * so the include chain skips mm_malloc and we still get the vector
 * intrinsics we actually use. */
#define _MM_MALLOC_H_INCLUDED
#include <immintrin.h>
#include <stdint.h>

/* ---- scalar fallbacks reused for row tails ------------------------------ */

static inline uint32_t scalar_mul8(uint32_t a, uint32_t b) {
    uint32_t t = a * b + 128u;
    return (t + (t >> 8)) >> 8;
}

static inline void scalar_copy_pixel(uint32_t *dst, uint32_t src) {
    *dst = src | 0xff000000u;
}

static inline void scalar_alpha_pixel(uint32_t *dst, uint32_t s, uint32_t ga) {
    uint32_t sa = scalar_mul8((s >> 24) & 0xffu, ga);
    if (sa == 0)   return;
    if (sa == 255) { *dst = s | 0xff000000u; return; }

    uint32_t d = *dst;
    uint32_t inv = 255u - sa;

    uint32_t sr = (s >> 16) & 0xffu, sg = (s >> 8) & 0xffu, sb = s & 0xffu;
    uint32_t dr = (d >> 16) & 0xffu, dg = (d >> 8) & 0xffu, db = d & 0xffu;

    uint32_t r = scalar_mul8(sr, sa) + scalar_mul8(dr, inv);
    uint32_t g = scalar_mul8(sg, sa) + scalar_mul8(dg, inv);
    uint32_t b = scalar_mul8(sb, sa) + scalar_mul8(db, inv);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *dst = 0xff000000u | (r << 16) | (g << 8) | b;
}

/* ==========================================================================
 * SSE2 kernels — legacy (non-VEX) encoding. Run on any x86_64 CPU.
 * ==========================================================================
 */

__attribute__((target("sse2")))
static inline __m128i mul8_sse2(__m128i ab) {
    __m128i v128 = _mm_set1_epi16(128);
    __m128i t = _mm_add_epi16(ab, v128);
    t = _mm_add_epi16(t, _mm_srli_epi16(t, 8));
    return _mm_srli_epi16(t, 8);
}

__attribute__((target("sse2")))
void blit_copy_row_sse2(uint32_t *dst, const uint32_t *src, int32_t n) {
    __m128i alpha_mask = _mm_set1_epi32((int) 0xff000000u);
    int32_t x = 0;
    for (; x + 4 <= n; x += 4) {
        __m128i s = _mm_loadu_si128((const __m128i *) (src + x));
        s = _mm_or_si128(s, alpha_mask);
        _mm_storeu_si128((__m128i *) (dst + x), s);
    }
    for (; x < n; x++) scalar_copy_pixel(&dst[x], src[x]);
}

__attribute__((target("sse2")))
void blit_alpha_row_sse2(uint32_t *dst, const uint32_t *src,
                         uint32_t ga, int32_t n) {
    if (ga == 0) return;

    __m128i zero = _mm_setzero_si128();
    __m128i v255 = _mm_set1_epi16(255);
    __m128i ga16 = _mm_set1_epi16((short) ga);
    __m128i alpha_mask = _mm_set1_epi32((int) 0xff000000u);

    int32_t x = 0;
    for (; x + 4 <= n; x += 4) {
        __m128i s = _mm_loadu_si128((const __m128i *) (src + x));
        __m128i d = _mm_loadu_si128((const __m128i *) (dst + x));

        /* Widen 4 packed 32-bit pixels into two halves of 16-bit lanes. */
        __m128i s_lo = _mm_unpacklo_epi8(s, zero);
        __m128i s_hi = _mm_unpackhi_epi8(s, zero);
        __m128i d_lo = _mm_unpacklo_epi8(d, zero);
        __m128i d_hi = _mm_unpackhi_epi8(d, zero);

        /* Broadcast each pixel's src-alpha (word index 3 / 7) across its
         * four channels. shufflelo/hi with _MM_SHUFFLE(3,3,3,3) replicates
         * element 3 into 0..3 of each 64-bit half. */
        __m128i a_lo = _mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
        a_lo = _mm_shufflehi_epi16(a_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i a_hi = _mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
        a_hi = _mm_shufflehi_epi16(a_hi, _MM_SHUFFLE(3, 3, 3, 3));

        /* Effective alpha = mul8(src_alpha, global_alpha). */
        __m128i ae_lo = mul8_sse2(_mm_mullo_epi16(a_lo, ga16));
        __m128i ae_hi = mul8_sse2(_mm_mullo_epi16(a_hi, ga16));

        __m128i inv_lo = _mm_sub_epi16(v255, ae_lo);
        __m128i inv_hi = _mm_sub_epi16(v255, ae_hi);

        /* Two independent mul8's summed — matches the scalar formula
         * mul8(sc, ae) + mul8(dc, inv) exactly, including ±1-LSB rounding
         * quirks, so SSE2 ↔ scalar produce identical pixels. */
        __m128i r_lo = _mm_add_epi16(mul8_sse2(_mm_mullo_epi16(s_lo, ae_lo)),
                                     mul8_sse2(_mm_mullo_epi16(d_lo, inv_lo)));
        __m128i r_hi = _mm_add_epi16(mul8_sse2(_mm_mullo_epi16(s_hi, ae_hi)),
                                     mul8_sse2(_mm_mullo_epi16(d_hi, inv_hi)));

        __m128i packed = _mm_packus_epi16(r_lo, r_hi);
        _mm_storeu_si128((__m128i *) (dst + x),
                         _mm_or_si128(packed, alpha_mask));
    }
    for (; x < n; x++) scalar_alpha_pixel(&dst[x], src[x], ga);
}

/* ==========================================================================
 * AVX2 kernels — VEX-encoded, 256-bit. Gated by CPUID at boot; the
 * dispatcher never calls these on a CPU that lacks AVX2.
 * ==========================================================================
 */

__attribute__((target("avx2")))
static inline __m256i mul8_avx2(__m256i ab) {
    __m256i v128 = _mm256_set1_epi16(128);
    __m256i t = _mm256_add_epi16(ab, v128);
    t = _mm256_add_epi16(t, _mm256_srli_epi16(t, 8));
    return _mm256_srli_epi16(t, 8);
}

__attribute__((target("avx2")))
void blit_copy_row_avx2(uint32_t *dst, const uint32_t *src, int32_t n) {
    __m256i alpha_mask = _mm256_set1_epi32((int) 0xff000000u);
    int32_t x = 0;
    for (; x + 8 <= n; x += 8) {
        __m256i s = _mm256_loadu_si256((const __m256i *) (src + x));
        s = _mm256_or_si256(s, alpha_mask);
        _mm256_storeu_si256((__m256i *) (dst + x), s);
    }
    _mm256_zeroupper();
    /* Tail: hand off to SSE2 row which itself cleans up to scalar. */
    if (x < n) blit_copy_row_sse2(dst + x, src + x, n - x);
}

__attribute__((target("avx2")))
void blit_alpha_row_avx2(uint32_t *dst, const uint32_t *src,
                         uint32_t ga, int32_t n) {
    if (ga == 0) return;

    __m256i zero = _mm256_setzero_si256();
    __m256i v255 = _mm256_set1_epi16(255);
    __m256i ga16 = _mm256_set1_epi16((short) ga);
    __m256i alpha_mask = _mm256_set1_epi32((int) 0xff000000u);

    int32_t x = 0;
    for (; x + 8 <= n; x += 8) {
        __m256i s = _mm256_loadu_si256((const __m256i *) (src + x));
        __m256i d = _mm256_loadu_si256((const __m256i *) (dst + x));

        /* AVX2 unpack/shuffle/pack all run per 128-bit lane, so the same
         * recipe the SSE2 path uses for 4 pixels works here for 8 — low
         * lane handles pixels 0..3, high lane handles 4..7 in parallel
         * with no cross-lane permute. */
        __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
        __m256i s_hi = _mm256_unpackhi_epi8(s, zero);
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i a_lo = _mm256_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
        a_lo = _mm256_shufflehi_epi16(a_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m256i a_hi = _mm256_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
        a_hi = _mm256_shufflehi_epi16(a_hi, _MM_SHUFFLE(3, 3, 3, 3));

        __m256i ae_lo = mul8_avx2(_mm256_mullo_epi16(a_lo, ga16));
        __m256i ae_hi = mul8_avx2(_mm256_mullo_epi16(a_hi, ga16));

        __m256i inv_lo = _mm256_sub_epi16(v255, ae_lo);
        __m256i inv_hi = _mm256_sub_epi16(v255, ae_hi);

        __m256i r_lo = _mm256_add_epi16(
                           mul8_avx2(_mm256_mullo_epi16(s_lo, ae_lo)),
                           mul8_avx2(_mm256_mullo_epi16(d_lo, inv_lo)));
        __m256i r_hi = _mm256_add_epi16(
                           mul8_avx2(_mm256_mullo_epi16(s_hi, ae_hi)),
                           mul8_avx2(_mm256_mullo_epi16(d_hi, inv_hi)));

        __m256i packed = _mm256_packus_epi16(r_lo, r_hi);
        _mm256_storeu_si256((__m256i *) (dst + x),
                            _mm256_or_si256(packed, alpha_mask));
    }
    _mm256_zeroupper();
    if (x < n) blit_alpha_row_sse2(dst + x, src + x, ga, n - x);
}
