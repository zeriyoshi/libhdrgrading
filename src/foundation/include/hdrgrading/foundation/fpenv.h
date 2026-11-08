/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_FPENV_H
#define HDRG_FOUNDATION_FPENV_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Pins the calling thread's floating-point control state to the IEEE default for the duration of
 * library work (the determinism level L0): round to nearest even, no flush-to-zero, no
 * denormals-are-zero, traps off. This is also the WASM and default ARM state, so the host's
 * FTZ/DAZ or rounding mode cannot change a result.
 *
 *   x86 / x86-64 (SSE)  MXCSR control bits = 0x1F80 (RC = nearest, FTZ = DAZ = 0, all masked);
 *                       the x87 control word is left alone (float/double math uses SSE)
 *   AArch64             FPCR = 0 (FZ, FZ16, RMode, DN, AHP, trap enables, FEAT_AFP FIZ/AH/NEP);
 *                       GCC/Clang via mrs/msr, MSVC via _ReadStatusReg/_WriteStatusReg
 *   anything else       <fenv.h> rounding mode only (FE_TONEAREST), e.g. WASM, 32-bit ARM
 *
 * enter saves the state and writes the register only when it differs from the pinned state;
 * leave writes the saved state back only when enter changed it. The common case (the host
 * already runs in the default state) therefore costs one register read per call, and nested
 * enter/leave pairs are no-ops. Pairs must be properly nested on the same thread. Status
 * (exception) flags raised in between are not preserved.
 */
typedef struct {
    uint64_t saved;
    bool changed;
} hdrg_fpenv_t;

extern void hdrg_fpenv_enter(hdrg_fpenv_t *env);
extern void hdrg_fpenv_leave(const hdrg_fpenv_t *env);

#endif /* HDRG_FOUNDATION_FPENV_H */
