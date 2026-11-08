/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrgrading/foundation/fpenv.h"

#if defined(__x86_64__) || defined(_M_X64) || (defined(__i386__) && defined(__SSE__)) ||           \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#define HDRG_FPENV_X86 1
#include <xmmintrin.h>
#elif defined(__aarch64__)
#define HDRG_FPENV_AARCH64_GNU 1
#elif defined(_M_ARM64)
#define HDRG_FPENV_AARCH64_MSVC 1
#include <intrin.h>
#ifndef ARM64_FPCR
#define ARM64_FPCR ARM64_SYSREG(3, 3, 4, 4, 0)
#endif
#else
#define HDRG_FPENV_FENV 1
#include <fenv.h>
#endif

#if defined(HDRG_FPENV_X86)
/* bits 0-5 are status flags; 6 DAZ, 7-12 exception masks, 13-14 RC, 15 FTZ */
#define HDRG_FPENV_CONTROL_MASK UINT64_C(0xffc0)
#define HDRG_FPENV_PINNED       UINT64_C(0x1f80)
#elif defined(HDRG_FPENV_FENV) && defined(FE_TONEAREST)
#define HDRG_FPENV_CONTROL_MASK UINT64_MAX
#define HDRG_FPENV_PINNED       ((uint64_t)FE_TONEAREST)
#else
/* FPCR has no status bits: every defined bit is a control that the pinned state clears */
#define HDRG_FPENV_CONTROL_MASK UINT64_MAX
#define HDRG_FPENV_PINNED       UINT64_C(0)
#endif

#if defined(HDRG_FPENV_X86)
static inline uint64_t hdrg_fpenv_read(void)
{
    return (uint64_t)_mm_getcsr();
}

static inline void hdrg_fpenv_write(uint64_t value)
{
    _mm_setcsr((unsigned int)value);
}
#elif defined(HDRG_FPENV_AARCH64_GNU)
static inline uint64_t hdrg_fpenv_read(void)
{
    uint64_t value;

    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));

    return value;
}

static inline void hdrg_fpenv_write(uint64_t value)
{
    __asm__ __volatile__("msr fpcr, %0" : : "r"(value) : "memory");
}
#elif defined(HDRG_FPENV_AARCH64_MSVC)
static inline uint64_t hdrg_fpenv_read(void)
{
    return (uint64_t)_ReadStatusReg(ARM64_FPCR);
}

static inline void hdrg_fpenv_write(uint64_t value)
{
    _WriteStatusReg(ARM64_FPCR, (int64_t)value);
}
#elif defined(FE_TONEAREST)
static inline uint64_t hdrg_fpenv_read(void)
{
    const int mode = fegetround();

    /* an undeterminable mode (< 0) reads as pinned, so it is never rewritten */
    return (mode < 0) ? HDRG_FPENV_PINNED : (uint64_t)mode;
}

static inline void hdrg_fpenv_write(uint64_t value)
{
    (void)fesetround((int)value);
}
#else
static inline uint64_t hdrg_fpenv_read(void)
{
    return HDRG_FPENV_PINNED;
}

static inline void hdrg_fpenv_write(uint64_t value)
{
    (void)value;
}
#endif

extern void hdrg_fpenv_enter(hdrg_fpenv_t *env)
{
    const uint64_t current = hdrg_fpenv_read();

    env->saved = current;
    env->changed = (current & HDRG_FPENV_CONTROL_MASK) != HDRG_FPENV_PINNED;
    if (env->changed) {
        hdrg_fpenv_write((current & ~HDRG_FPENV_CONTROL_MASK) | HDRG_FPENV_PINNED);
    }
}

extern void hdrg_fpenv_leave(const hdrg_fpenv_t *env)
{
    if (env->changed) {
        hdrg_fpenv_write(env->saved);
    }
}
