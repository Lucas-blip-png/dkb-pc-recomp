// FP control on AArch64. The guest FPCR is the host FPCR: lifted FP code runs with the guest's rounding mode,
// flush-to-zero, default-NaN and AHP settings active in hardware, so a read or write here is the guest's own value
// and nothing has to be translated. Inline on purpose, like fp_x86.h: the FP/SIMD lowering touches it on every
// operation that depends on the guest FPCR.
#pragma once
#include <stdint.h>

#define PLAT_FP_RC_MASK (3u << 22)   // RMode: 0 nearest, 1 +inf, 2 -inf, 3 toward zero
#define PLAT_FP_FZ (1u << 24)        // flush denormals to zero
#define PLAT_FP_DAZ 0u               // no separate denormals-are-zero flag; FPCR.FZ covers both

static inline uint32_t plat_fp_get(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (uint32_t)v;
}

static inline void plat_fp_set(uint32_t v) { __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)v)); }
