#pragma once

/* Per-CPU SIMD enable + runtime feature flags. The rest of the kernel is
 * built with -mgeneral-regs-only, so XMM/YMM use is confined to TUs that
 * opt back in (see compositor/blit_simd.c). Before any such TU executes a
 * vector instruction, simd_cpu_init() must have run on that CPU to:
 *   - clear CR0.EM / set CR0.MP    (SSE not trapped as x87)
 *   - set CR4.OSFXSR / OSXMMEXCPT  (legalises FXSAVE/FXRSTOR, XMM excs)
 *   - set CR4.OSXSAVE + XCR0.AVX   (legalises 256-bit YMM, if AVX present)
 *
 * No FPU state save on context switch today — the whole kernel + libc are
 * -mgeneral-regs-only, so nothing else touches XMM. The compositor is the
 * single SIMD consumer, a kernel-only thread, and its SIMD state survives
 * preemption precisely because no other scheduled code clobbers XMM/YMM.
 */

#include <stdint.h>

extern volatile int g_simd_sse2;
extern volatile int g_simd_avx;
extern volatile int g_simd_avx2;

static inline int simd_has_sse2(void) { return g_simd_sse2; }
static inline int simd_has_avx(void)  { return g_simd_avx; }
static inline int simd_has_avx2(void) { return g_simd_avx2; }

/* Called once per CPU after the IDT is loaded. is_bsp=1 on the BSP, which
 * also records the feature bits into the globals above; APs re-detect to
 * confirm the common subset but trust the BSP-published flags. */
void simd_cpu_init(int is_bsp);
