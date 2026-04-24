#include "simd.h"

#include "../../kprintf.h"

#include <stdint.h>

volatile int g_simd_sse2;
volatile int g_simd_avx;
volatile int g_simd_avx2;

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(subleaf));
}

#define CR0_MP   (1ull <<  1)
#define CR0_EM   (1ull <<  2)
#define CR4_OSFXSR      (1ull <<  9)
#define CR4_OSXMMEXCPT  (1ull << 10)
#define CR4_OSXSAVE     (1ull << 18)

#define XCR0_X87  (1ull << 0)
#define XCR0_SSE  (1ull << 1)
#define XCR0_AVX  (1ull << 2)

void simd_cpu_init(int is_bsp) {
    uint32_t a, b, c, d;

    cpuid(1, 0, &a, &b, &c, &d);
    int has_sse2  = (d >> 26) & 1;
    int has_xsave = (c >> 26) & 1;
    int has_avx   = (c >> 28) & 1;

    cpuid(7, 0, &a, &b, &c, &d);
    int has_avx2  = (b >> 5) & 1;

    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~CR0_EM;
    cr0 |=  CR0_MP;
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (has_xsave) cr4 |= CR4_OSXSAVE;
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));

    /* AVX requires OSXSAVE + XCR0.AVX. Without both, YMM instructions
     * raise #UD — demote the flags so the dispatcher picks SSE2 or the
     * scalar fallback. */
    int enable_avx  = has_avx  && has_xsave;
    int enable_avx2 = has_avx2 && enable_avx;

    if (enable_avx) {
        uint32_t lo, hi;
        __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        uint64_t xcr0 = ((uint64_t) hi << 32) | lo;
        xcr0 |= XCR0_X87 | XCR0_SSE | XCR0_AVX;
        lo = (uint32_t) xcr0;
        hi = (uint32_t) (xcr0 >> 32);
        __asm__ volatile ("xsetbv" :: "a"(lo), "d"(hi), "c"(0));
    }

    if (is_bsp) {
        g_simd_sse2 = has_sse2;
        g_simd_avx  = enable_avx;
        g_simd_avx2 = enable_avx2;
        kprintf("[simd] bsp features sse2=%d avx=%d avx2=%d xsave=%d\n",
                has_sse2, enable_avx, enable_avx2, has_xsave);
    }
}
