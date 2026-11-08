/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_MATH_H
#define HDRG_FOUNDATION_MATH_H

/*
 * Deterministic replacements for the libm transcendental functions.
 * Nothing here calls libm: the functions use only IEEE-exact operations (+ - * /, comparisons,
 * float <-> integer conversion, bit manipulation), evaluate in double and round once to float,
 * so a result depends on the build alone, never on the C library it runs against (the output is
 * bit-identical across arm64 / x86-64, GCC / Clang, -O0 / -O2 as long as -ffp-contract=off).
 * Results assume the IEEE default environment (round to nearest, no FTZ/DAZ): call
 * hdrg_fpenv_enter() first.
 *
 * Accuracy against libm double. The double kernels are accurate to about 1e-14, so the float
 * result is the correctly rounded one except within ~1e-14 of a rounding midpoint:
 *   exp2f, log2f, cbrtf  correctly rounded for every float input (exhaustive check)
 *   expf, powf           max 0.5000002 ulp (a few near-midpoint inputs in 1e9 round the other way)
 *   all of the above     relative error <= 6e-8 (2^-24) for normal results
 *   sRGB / PQ curves     <= 1.3e-4 LSB at 8, 10 and 12-bit code values, <= 2e-3 LSB at 16-bit
 * tests/unit/test_common_math.c asserts <= 0.5000005 ulp on dense sweeps and <= 0.01 LSB.
 *
 * Special inputs. "NaN" is the canonical quiet NaN (bits 0x7FC00000), whatever the input payload.
 * Subnormal inputs and results are exact (as far as float allows) in the IEEE environment.
 *   exp2f, expf   NaN -> NaN, -inf -> +0, +inf -> +inf; overflow -> +inf, underflow rounds to a
 *                 subnormal or +0 (exp2f(-150) = +0, exp2f(-149) = 2^-149); exact at integers
 *   log2f         NaN or x < 0 -> NaN, +-0 -> -inf, +inf -> +inf; exact at powers of two
 *   powf (x >= 0) y = +-0 -> 1 (any x, even NaN); x = 1 -> 1 (any y, even NaN);
 *                 otherwise NaN in either argument or x < 0 -> NaN (no odd-integer rule);
 *                 x = +-0 -> +0 for y > 0, +inf for y < 0;
 *                 x = +inf -> +inf for y > 0, +0 for y < 0;
 *                 y = +-inf follows the limit (0 < x < 1: +0 / +inf, x > 1: +inf / +0)
 *   cbrtf         odd: cbrtf(-x) = -cbrtf(x); NaN -> NaN, +-0 -> +-0, +-inf -> +-inf;
 *                 exact at cubes of powers of two and at small integer cubes
 */

/* 2^x */
extern float hdrg_math_exp2f(float x);

/* e^x */
extern float hdrg_math_expf(float x);

/* log2(x) */
extern float hdrg_math_log2f(float x);

/* x^y for x >= 0 */
extern float hdrg_math_powf(float x, float y);

/* Real cube root (negative x gives a negative result). */
extern float hdrg_math_cbrtf(float x);

/*
 * sin(pi x), for the resampling kernels of the filters. Exact zeros at the integers (+0, -0 for
 * the negative ones; |x| >= 2^23 is an integer) and +-1 at the half-integers; NaN, +-inf -> NaN.
 * The reduction x - round(x) is exact, so the error stays within 0.5000005 ulp at any x.
 */
extern float hdrg_math_sinpif(float x);

/*
 * IEC 61966-2-1 sRGB curves (shared by sRGB and Display P3). Specified on [0, 1]; outside it the
 * two segments continue (x <= threshold uses the linear segment, so negative values stay linear,
 * and values above 1 follow the power segment). NaN -> NaN, +inf -> +inf, -inf -> -inf. Both are
 * monotonic on each segment; the IEC constants leave the OETF a 1.7e-6 downward step at its
 * threshold 0.0031308 (the exact formula has the same step; it is 0.007 LSB at 12 bits).
 */
extern float hdrg_math_srgb_eotf(float encoded);
extern float hdrg_math_srgb_oetf(float linear);

/*
 * SMPTE ST 2084 PQ. hdrg_math_pq_eotf: signal [0, 1] -> luminance in nits [0, 10000];
 * hdrg_math_pq_oetf (the inverse EOTF): nits -> signal. Inputs are clamped to the domain first,
 * so negative inputs give 0 nits / the 0-nit signal (c1^m2 = 7.3e-7) and inputs above it give
 * 10000 nits / 1. The EOTF is exactly 0 below the signal c1^m2. NaN -> NaN.
 */
extern float hdrg_math_pq_eotf(float signal);
extern float hdrg_math_pq_oetf(float nits);

#endif /* HDRG_FOUNDATION_MATH_H */
