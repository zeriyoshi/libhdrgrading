/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stddef.h>
#include <string.h>

#include "hdrgrading/hdrgrading.h"

#include "kernels.h"

#if defined(HDRG_KERNELS_X86) && defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#elif defined(HDRG_KERNELS_X86)
#include <cpuid.h>
#endif
#if defined(HDRG_KERNELS_AVX512) && defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#ifdef HDRG_KERNELS_SVE
#include <sys/auxv.h>
#include <sys/prctl.h>
#endif

/* CPUID bits (Intel SDM Vol. 2A, CPUID), XCR0 states and the AArch64 hwcap of SVE */
#define HDRG_CPUID1_ECX_SSE41   (1u << 19)
#define HDRG_CPUID1_ECX_FMA     (1u << 12)
#define HDRG_CPUID1_ECX_OSXSAVE (1u << 27)
#define HDRG_CPUID1_ECX_AVX     (1u << 28)
#define HDRG_CPUID7_EBX_AVX2    (1u << 5)
#define HDRG_CPUID7_EBX_AVX512                                                                     \
    ((1u << 16) | (1u << 17) | (1u << 30) | (1u << 31))              /* F, DQ, BW, VL */
#define HDRG_CPUID7_ECX_AVX512 ((1u << 6) | (1u << 11) | (1u << 12)) /* VBMI2, VNNI, BITALG */
#define HDRG_XCR0_SSE_AVX      UINT64_C(0x6)
#define HDRG_XCR0_AVX512       UINT64_C(0xE6)      /* SSE, AVX, opmask, ZMM_Hi256, Hi16_ZMM */
#define HDRG_HWCAP_SVE         (UINT64_C(1) << 22) /* AT_HWCAP of Linux on AArch64 */
#define HDRG_SVE_MIN_VL        32                  /* bytes: below 256 bits NEON is as wide */

typedef struct {
    const char *name;
    uint8_t tier;
} hdrg_tier_name_t;

static const hdrg_tier_name_t hdrg_tier_names[] = {
    {"scalar", HDRG_SIMD_SCALAR},   {"sse41", HDRG_SIMD_SSE41}, {"avx2", HDRG_SIMD_AVX2},
    {"avx512", HDRG_SIMD_AVX512},   {"neon", HDRG_SIMD_NEON},   {"sve", HDRG_SIMD_SVE},
    {"wasm128", HDRG_SIMD_WASM128},
};

/*
 * The tiers of this build's architecture, lowest first; the last one is
 * provisional where there is one (chosen only by a cap naming it)
 */
#if defined(HDRG_KERNELS_X86)
static const uint8_t hdrg_chain[] = {HDRG_SIMD_SCALAR, HDRG_SIMD_SSE41, HDRG_SIMD_AVX2,
                                     HDRG_SIMD_AVX512};
static const size_t hdrg_chain_stable = 3;
#elif defined(HDRG_KERNELS_NEON)
static const uint8_t hdrg_chain[] = {HDRG_SIMD_SCALAR, HDRG_SIMD_NEON, HDRG_SIMD_SVE};
static const size_t hdrg_chain_stable = 2;
#elif defined(HDRG_KERNELS_WASM128)
static const uint8_t hdrg_chain[] = {HDRG_SIMD_SCALAR, HDRG_SIMD_WASM128};
static const size_t hdrg_chain_stable = 2;
#else
static const uint8_t hdrg_chain[] = {HDRG_SIMD_SCALAR};
static const size_t hdrg_chain_stable = 1;
#endif

#if defined(HDRG_KERNELS_X86) && defined(_MSC_VER) && !defined(__clang__)
static void hdrg_cpuid(uint32_t leaf, uint32_t sub, uint32_t regs[4])
{
    uint8_t i;
    int out[4];

    __cpuidex(out, (int)leaf, (int)sub);

    for (i = 0; i < 4; i++) {
        regs[i] = (uint32_t)out[i];
    }
}

static uint64_t hdrg_xcr0(void)
{
    return (uint64_t)_xgetbv(0);
}
#elif defined(HDRG_KERNELS_X86)
static void hdrg_cpuid(uint32_t leaf, uint32_t sub, uint32_t regs[4])
{
    unsigned int a, b, c, d;

    a = 0;
    b = 0;
    c = 0;
    d = 0;
    __cpuid_count(leaf, sub, a, b, c, d);

    regs[0] = a;
    regs[1] = b;
    regs[2] = c;
    regs[3] = d;
}

/* inline asm: the _xgetbv intrinsic needs the xsave target feature on GCC / Clang */
static uint64_t hdrg_xcr0(void)
{
    uint32_t lo, hi;

    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));

    return ((uint64_t)hi << 32) | lo;
}
#endif

#if defined(HDRG_KERNELS_AVX512) && defined(__APPLE__)
/* the OS saves the AVX-512 state: macOS enables it on first use, so XCR0 does not show it yet */
static bool hdrg_os_avx512(void)
{
    int32_t value;
    size_t size;

    value = 0;
    size = sizeof(value);

    return sysctlbyname("hw.optional.avx512f", &value, &size, NULL, 0) == 0 && value != 0;
}
#elif defined(HDRG_KERNELS_X86)
/* the OS saves the AVX-512 state (opmask, ZMM_Hi256, Hi16_ZMM) */
static bool hdrg_os_avx512(void)
{
    return (hdrg_xcr0() & HDRG_XCR0_AVX512) == HDRG_XCR0_AVX512;
}
#endif

#if defined(HDRG_KERNELS_X86)
/*
 * SSE4.1; AVX2 + FMA with the OS saving the AVX state; AVX-512 of the Ice
 * Lake class (F / DQ / BW / VL + VBMI2 / VNNI / BITALG) on top
 */
static bool hdrg_cpu_runs(uint8_t tier)
{
    uint32_t leaf1[4], leaf7[4], max_leaf[4];

    hdrg_cpuid(0, 0, max_leaf);
    hdrg_cpuid(1, 0, leaf1);

    if (tier == HDRG_SIMD_SSE41) {
        return (leaf1[2] & HDRG_CPUID1_ECX_SSE41) != 0;
    }

    if ((tier != HDRG_SIMD_AVX2 && tier != HDRG_SIMD_AVX512) || max_leaf[0] < 7 ||
        (leaf1[2] & (HDRG_CPUID1_ECX_FMA | HDRG_CPUID1_ECX_OSXSAVE | HDRG_CPUID1_ECX_AVX)) !=
            (HDRG_CPUID1_ECX_FMA | HDRG_CPUID1_ECX_OSXSAVE | HDRG_CPUID1_ECX_AVX)) {
        return false;
    }

    hdrg_cpuid(7, 0, leaf7);

    if ((leaf7[1] & HDRG_CPUID7_EBX_AVX2) == 0 ||
        (hdrg_xcr0() & HDRG_XCR0_SSE_AVX) != HDRG_XCR0_SSE_AVX) {
        return false;
    }

    if (tier == HDRG_SIMD_AVX2) {
        return true;
    }

    return (leaf7[1] & HDRG_CPUID7_EBX_AVX512) == HDRG_CPUID7_EBX_AVX512 &&
           (leaf7[2] & HDRG_CPUID7_ECX_AVX512) == HDRG_CPUID7_ECX_AVX512 && hdrg_os_avx512();
}
#elif defined(HDRG_KERNELS_NEON)
/* NEON is the AArch64 baseline; SVE where the kernel reports it with a vector of 256 bits or more
 */
static bool hdrg_cpu_runs(uint8_t tier)
{
#ifdef HDRG_KERNELS_SVE
    if (tier == HDRG_SIMD_SVE) {
        return ((uint64_t)getauxval(AT_HWCAP) & HDRG_HWCAP_SVE) != 0 &&
               (prctl(PR_SVE_GET_VL) & PR_SVE_VL_LEN_MASK) >= HDRG_SVE_MIN_VL;
    }
#endif

    return tier == HDRG_SIMD_NEON;
}
#elif defined(HDRG_KERNELS_WASM128)
/* a module built with SIMD128 only instantiates where the engine has it */
static bool hdrg_cpu_runs(uint8_t tier)
{
    return tier == HDRG_SIMD_WASM128;
}
#endif

extern const hdrg_kernels_t *hdrg_kernels_get(uint8_t tier)
{
    if (tier == HDRG_SIMD_SCALAR) {
        return &hdrg_kernels_scalar;
    }

#if defined(HDRG_KERNELS_X86)
    if (tier == HDRG_SIMD_SSE41 && hdrg_cpu_runs(tier)) {
        return &hdrg_kernels_sse41;
    }

    if (tier == HDRG_SIMD_AVX2 && hdrg_cpu_runs(tier)) {
        return &hdrg_kernels_avx2;
    }
#endif

#if defined(HDRG_KERNELS_AVX512)
    if (tier == HDRG_SIMD_AVX512 && hdrg_cpu_runs(tier)) {
        return &hdrg_kernels_avx512;
    }
#endif

#if defined(HDRG_KERNELS_NEON)
    if (tier == HDRG_SIMD_NEON && hdrg_cpu_runs(tier)) {
        return &hdrg_kernels_neon;
    }
#endif

#if defined(HDRG_KERNELS_SVE)
    if (tier == HDRG_SIMD_SVE && hdrg_cpu_runs(tier)) {
        return &hdrg_kernels_sve;
    }
#endif

#if defined(HDRG_KERNELS_WASM128)
    if (tier == HDRG_SIMD_WASM128 && hdrg_cpu_runs(tier)) {
        return &hdrg_kernels_wasm128;
    }
#endif

    return NULL;
}

extern const hdrg_kernels_t *hdrg_kernels_select(uint8_t cap)
{
    const hdrg_kernels_t *kernels;
    size_t limit, i;

    limit = hdrg_chain_stable - 1u;

    if (cap != 0) {
        i = 0;
        while (i < sizeof(hdrg_chain) / sizeof(hdrg_chain[0]) && hdrg_chain[i] != cap) {
            i++;
        }

        limit = (i < sizeof(hdrg_chain) / sizeof(hdrg_chain[0])) ? i : 0;
    }

    for (i = limit; i > 0; i--) {
        kernels = hdrg_kernels_get(hdrg_chain[i]);
        if (kernels != NULL) {
            return kernels;
        }
    }

    return &hdrg_kernels_scalar;
}

extern uint8_t hdrg_kernels_parse(const char *name)
{
    size_t i;

    if (name == NULL) {
        return 0;
    }

    for (i = 0; i < sizeof(hdrg_tier_names) / sizeof(hdrg_tier_names[0]); i++) {
        if (strcmp(name, hdrg_tier_names[i].name) == 0) {
            return hdrg_tier_names[i].tier;
        }
    }

    return 0;
}

extern const char *hdrg_simd_name(uint8_t tier)
{
    size_t i;

    for (i = 0; i < sizeof(hdrg_tier_names) / sizeof(hdrg_tier_names[0]); i++) {
        if (hdrg_tier_names[i].tier == tier) {
            return hdrg_tier_names[i].name;
        }
    }

    return NULL;
}
